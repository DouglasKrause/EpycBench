// Included with the ordinary worker layer to share placement and admission.
namespace {
using StreamPlans = std::vector<std::array<RankPlan,2>>;
using Json = nlohmann::json;

auto invalid_stream(std::string_view why) -> core::Error {
    return failure("runtime.package.stream_invalid",why);
}

auto stream_plans(std::span<const Role> roles, std::size_t chunks,
                  std::optional<std::size_t>* short_partition_stage = nullptr)
    -> std::expected<StreamPlans,core::Error> {
    if (chunks<2U || chunks>64U || roles.size()!=2U ||
        roles[0].cpus.size()!=1U || roles[1].cpus.size()!=1U || roles[0].cpus==roles[1].cpus ||
        !roles[0].links.empty() || roles[1].links.size()!=1U ||
        roles[0].request.repetitions!=roles[1].request.repetitions ||
        roles[0].request.repetitions==0U || roles[0].request.repetitions>64U/chunks ||
        std::ranges::any_of(roles,[](const auto& role) {
            return role.module.collective() || role.request.hardware_counters ||
                !kernel::packages::stream_input_placement_supported(role.input_placement.policy,
                                                                     role.input_placement.targets.size());
        }))
        return std::unexpected{invalid_stream("Streaming needs two distinct single-rank stages, ordinary bind, one link and at most 64 chunk calls per stage")};
    StreamPlans plans(chunks*static_cast<std::size_t>(roles[0].request.repetitions));
    for (std::size_t s=0;s<roles.size();++s) {
        const auto& role=roles[s];
        auto whole=role.request;
        whole.repetitions=1U;
        const auto full=layout(role.module,whole,UINT64_MAX);
        if (!full) return std::unexpected{full.error()};
        if (full->partition_begin!=0 || full->partition_count<chunks) {
            if (short_partition_stage && full->partition_begin == 0U && full->partition_count > 0U)
                *short_partition_stage = s;
            return std::unexpected{invalid_stream("Task layout cannot split its complete logical range into positive chunks")};
        }
        const auto base=full->partition_count/chunks;
        const auto remainder=full->partition_count%chunks;
        for (std::size_t c=0;c<plans.size();++c) {
            auto request=whole;
            const auto part=c%chunks;
            request.partition_begin=part*base+std::min(part,remainder);
            request.partition_count=base+(part<remainder ? 1U:0U);
            const auto accepted=layout(role.module,request);
            if (!accepted) return std::unexpected{accepted.error()};
            plans[c][s]={s,std::move(request),*accepted,{},{},0U,{}};
        }
    }
    const auto& link=roles[1].links.front();
    for (const auto& row:plans) {
        const auto* destination=find_region(row[1].layout,link.input,EPYCBENCH_PACKAGE_INPUT);
        const auto* source=find_region(row[0].layout,link.output,EPYCBENCH_PACKAGE_OUTPUT);
        if (!source || !destination || source->bytes!=destination->bytes)
            return std::unexpected{invalid_stream("Every streaming chunk requires matching source and destination regions")};
    }
    return plans;
}

auto stream_mapping(void*,const AllocationRequest& request) -> std::expected<Allocation,core::Error> {
    const auto allocated=request.bytes+request.alignment;
    auto mapping=AnonymousMapping::create(allocated);
    if (!mapping) return std::unexpected{mapping.error()};
    auto owner=std::make_shared<AnonymousMapping>(std::move(*mapping));
    const auto address=reinterpret_cast<std::uintptr_t>(owner->data());
    const auto offset=(request.alignment-address%request.alignment)%request.alignment;
    const auto bytes=owner->writable_bytes().subspan(offset,request.bytes);
    return Allocation{std::move(owner),bytes};
}

auto stream_native(const Role& role,std::uint64_t page) -> std::expected<NativeResource,core::Error> {
    const auto node=memory_node(role,0U);
    auto nodes=NativeNumaMask::from_node_set(core::LinuxNumaNodeSet{node});
    if (!nodes) return std::unexpected{nodes.error()};
    std::optional<NativeNumaMask> input;
    if (!role.input_placement.targets.empty()) {
        auto mask=NativeNumaMask::from_node_set(core::LinuxNumaNodeSet{role.input_placement.targets.front()});
        if (!mask) return std::unexpected{mask.error()};
        input.emplace(std::move(*mask));
    }
    return NativeResource{page,node,std::move(*nodes),{},role.input_placement,std::move(input),false,{}};
}

struct StreamSlotProvider {
    ResourceProvider local;
    NativeResource* native;
    Allocation* slot;
    std::string_view output;

    static auto allocate(void* context,const AllocationRequest& request) -> std::expected<Allocation,core::Error> {
        const auto& provider=*static_cast<const StreamSlotProvider*>(context);
        if (!provider.slot || request.reference_output || request.kind!=EPYCBENCH_PACKAGE_OUTPUT || request.name!=provider.output)
            return provider.local.allocate(provider.local.context,request);
        const auto bytes=provider.slot->bytes.first(request.bytes);
        if (provider.native) {
            const auto node=provider.native->node;
            provider.native->working.push_back({std::string{request.name},
                std::static_pointer_cast<AnonymousMapping>(provider.slot->owner),bytes,request.kind,node,InputPolicy::bind,{node}});
        }
        return Allocation{provider.slot->owner,bytes};
    }
};

struct StreamRing {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::size_t,2> sequence{0U,1U};
    std::size_t consumed{};
    bool failed{};

    auto acquire(std::size_t chunk,bool consumer,bool& waited) -> bool {
        std::unique_lock lock{mutex};
        const auto target=chunk+(consumer ? 1U:0U);
        const auto& slot=sequence[chunk%2U];
        waited=slot!=target && !failed;
        changed.wait(lock,[&] { return failed || slot==target; });
        return !failed;
    }

    void advance(std::size_t chunk,bool consumer) {
        {
            const std::scoped_lock lock{mutex};
            sequence[chunk%2U]=chunk+(consumer ? 2U:1U);
            if (consumer) consumed=chunk+1U;
        }
        changed.notify_all();
    }

    auto drain(std::size_t count) -> bool {
        std::unique_lock lock{mutex};
        changed.wait(lock,[&] { return failed || consumed==count; });
        return !failed;
    }

    void abort() {
        {
            const std::scoped_lock lock{mutex};
            failed=true;
        }
        changed.notify_all();
    }

    [[nodiscard]] auto aborted() -> bool {
        const std::scoped_lock lock{mutex};
        return failed;
    }
};

struct StreamStageState {
    std::optional<PreparedTrial> prepared;
    std::optional<PreparedTrial::StreamCallback> callback;
    RankResources resources;
};

struct StreamHandoffDigest {
    std::uint64_t bytes;
    core::Sha256Digest sha256;
};

auto hash_stream_handoff(std::span<std::array<StreamStageState, 2>> states, const StageLink& link, bool reference)
    -> std::expected<StreamHandoffDigest, core::Error> {
    core::BinaryDigest hash;
    std::uint64_t bytes{};
    for (auto& chunk : states) {
        const auto part = reference ? chunk[0].prepared->stream_reference(link.output)
                                    : std::span<const std::byte>{chunk[1].prepared->stream_input(link.input)};
        if (part.empty() || part.size() > core::Sha256Stream::maximum_input_bytes - bytes || !hash.append(part))
            return std::unexpected{invalid_stream("Cannot hash logical stream handoff")};
        bytes += part.size();
    }
    const auto digest = hash.finish();
    if (!digest) return std::unexpected{invalid_stream("Cannot finalize logical stream handoff hash")};
    return StreamHandoffDigest{bytes, *digest};
}

auto estimate_stream(const StreamPlans& plans,std::span<const Role> roles,std::uint64_t page,
                     std::uint64_t budget,std::size_t slot_bytes,std::size_t slot_alignment) -> std::expected<MemoryDemand,core::Error> {
    MemoryDemand demand;
    const auto add=[&](std::uint64_t bytes,std::uint64_t alignment,LinuxNumaNodeId node) {
        const auto charge=mapping_bytes(bytes,alignment,page);
        if (charge>budget-demand.peak) return false;
        demand.peak+=charge;
        demand.nodes[node]+=charge;
        return true;
    };
    for (const auto& chunk:plans) {
        for (std::size_t s=0;s<roles.size();++s) {
            const auto& role=roles[s];
            const auto node=memory_node(role,0U);
            const auto& l=chunk[s].layout;
            if (l.preparation_bytes && !add(l.preparation_bytes,page,node)) return std::unexpected{budget_error(budget)};
            for (const auto& region:std::span{l.regions}.first(l.region_count)) {
                const auto& link=roles[1].links.front();
                if (s==0U && region.kind==EPYCBENCH_PACKAGE_OUTPUT && region.name==std::string_view{link.output}) {
                    if (!add(region.bytes,region.alignment,node)) return std::unexpected{budget_error(budget)};
                    continue;
                }
                const auto input_node=region.kind==EPYCBENCH_PACKAGE_INPUT && !role.input_placement.targets.empty()
                    ? role.input_placement.targets.front():node;
                if (!add(region.bytes,region.alignment,input_node) ||
                    (region.kind==EPYCBENCH_PACKAGE_OUTPUT && !add(region.bytes,region.alignment,node)))
                    return std::unexpected{budget_error(budget)};
            }
        }
    }
    for (std::size_t slot=0;slot<2U;++slot)
        if (!add(slot_bytes,slot_alignment,memory_node(roles[0],0U))) return std::unexpected{budget_error(budget)};
    return demand;
}

auto prepare_stream_chunk(const Role& role,const RankPlan& rank, const StageLink& link,bool consumer,
                          std::size_t chunk,std::uint64_t page,std::uint64_t budget,const SuppliedStream* supplied,
                          Allocation& slot,std::span<const std::byte> source_reference,StreamStageState& state,
                          RankTrial& evidence) -> std::expected<void,core::Error> {
    if (supplied) {
        const auto services=fixture_services(const_cast<std::vector<SuppliedRank>*>(&supplied->chunks[chunk]),role,0U);
        if (!services) return std::unexpected{services.error()};
        state.resources.services=*services;
        evidence.placement=services->placement;
        evidence.placement.verified=false;
        auto ready=validate_supplied(evidence.placement,rank,role,services->clock);
        if (!ready) return ready;
    } else {
        auto native=stream_native(role,page);
        if (!native) return std::unexpected{native.error()};
        state.resources.native.emplace(std::move(*native));
        state.resources.services.resources={&*state.resources.native,NativeResource::allocate};
    }
    auto request=rank.request;
    request.supplied_workers=supplied!=nullptr;
    auto provider=state.resources.services.resources;
    if (!provider.allocate) provider={nullptr,stream_mapping};
    StreamSlotProvider slot_provider{provider,state.resources.native ? &*state.resources.native : nullptr,
                                    consumer ? nullptr:&slot,link.output};
    const InputBinding input{link.input,source_reference};
    const std::span<const InputBinding> inputs=consumer ? std::span{&input,1U}:std::span<const InputBinding>{};
    auto prepared=prepare(role.module,request,{&slot_provider,StreamSlotProvider::allocate},budget,
                          {inputs,{},&rank.layout});
    if (!prepared) return std::unexpected{prepared.error()};
    state.prepared.emplace(std::move(*prepared));
    if (state.resources.native) {
        const auto protection=state.resources.native->protect_inputs(consumer ? std::string_view{link.input}:std::string_view{});
        if (!protection) return protection;
        return state.resources.native->observe(evidence.placement,false);
    }
    return {};
}
} // namespace

auto inspect_stream_layouts(std::span<const Role> roles, std::size_t chunks, std::uint64_t page_bytes,
                            std::optional<std::size_t>* short_partition_stage)
    -> std::expected<Json,core::Error> {
    if (short_partition_stage) short_partition_stage->reset();
    if (page_bytes!=0U && (!std::has_single_bit(page_bytes) ||
        page_bytes>static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
        return std::unexpected{invalid_stream("Streaming partition inspection needs a valid page size")};
    auto planned=stream_plans(roles,chunks,short_partition_stage);
    if (!planned) return std::unexpected{planned.error()};
    Json geometry=Json::array();
    for (std::size_t c=0;c<planned->size();++c) {
        for (const auto& rank:planned->at(c)) {
            const auto& role=roles[rank.role_index];
            auto row=encode_layout(role,rank);
            const auto inspection=inspect_task(role.module,rank.request,rank.layout);
            if (!inspection) return std::unexpected{inspection.error()};
            if (*inspection) row["task_inspection"]=**inspection;
            row["chunk"]=c;
            row["pass"]=c/chunks;
            geometry.push_back(std::move(row));
        }
    }
    return geometry;
}

auto StreamEvidence::valid() const noexcept -> bool {
    if (error || !release_ns || !completion_ns || *release_ns>=*completion_ns || chunks_per_pass<2U ||
        !logical_handoff || logical_handoff->reference != logical_handoff->observed) return false;
    std::array last{*release_ns,*release_ns}, free_slot=last;
    for (std::size_t c=0;c<chunks.size();++c) {
        const auto& chunk=chunks[c];
        if (!chunk.handoff_ns || !std::ranges::all_of(chunk.ranks,&RankTrial::valid)) return false;
        const auto& producer=*chunk.ranks[0].trial;
        const auto& consumer=*chunk.ranks[1].trial;
        if (*producer.begin_ns<last[0] || *consumer.begin_ns<last[1] ||
            *producer.begin_ns<free_slot[c%2U] || *producer.end_ns>*chunk.handoff_ns ||
            (c%chunks_per_pass==0U && *producer.begin_ns<last[1]) ||
            *chunk.handoff_ns>*consumer.begin_ns || *consumer.end_ns>*completion_ns) return false;
        last={*producer.end_ns,*consumer.end_ns};
        free_slot[c%2U]=last[1];
    }
    return true;
}

auto run_stream(std::span<const Role> roles,std::size_t chunks,const SuppliedStream* supplied,
                const Json* admitted_geometry, PackageMonitor* monitor)
    -> std::expected<StreamEvidence,core::Error> {
    auto plans=stream_plans(roles,chunks);
    if (!plans) return std::unexpected{plans.error()};
    const auto item_count=plans->size();
    if (admitted_geometry) {
        if (!admitted_geometry->is_array() || admitted_geometry->size()!=item_count*roles.size())
            return std::unexpected{invalid_stream("Frozen stream geometry has the wrong chunk population")};
        for (std::size_t c=0;c<item_count;++c) {
            for (const auto& rank:plans->at(c)) {
                auto expected=encode_layout(roles[rank.role_index],rank);
                auto actual=admitted_geometry->at(c*roles.size()+rank.role_index);
                expected["chunk"]=c;
                expected["pass"]=c/chunks;
                actual.erase("task_inspection");
                if (actual!=expected)
                    return std::unexpected{failure("runtime.package.layout_unadmitted",
                        "Streaming callback layout differs from its frozen chunk",
                        {{"chunk",c},{"role",roles[rank.role_index].id}})};
            }
        }
    }
    if (supplied && (supplied->chunks.size()!=item_count || supplied->handoff_ns.size()!=item_count ||
        std::ranges::any_of(supplied->chunks,[](const auto& rows) { return rows.size()!=2U; })))
        return std::unexpected{invalid_stream("Every stream chunk in every repetition needs two supplied worker records and a handoff clock")};
    const auto native_page=supplied ? 0L : ::sysconf(_SC_PAGESIZE);
    const auto page=supplied ? supplied->chunks.front().front().placement.page_bytes :
        native_page>0 ? static_cast<std::uint64_t>(native_page):0U;
    if (!std::has_single_bit(page) || page>static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        return std::unexpected{invalid_stream("Cannot determine streaming page size")};
    const auto budget=supplied ? std::expected<std::uint64_t,core::Error>{default_region_budget}:native_memory_limit();
    if (!budget) return std::unexpected{budget.error()};
    const auto& link=roles[1].links.front();
    std::size_t slot_bytes{},slot_alignment{};
    for (const auto& rank:*plans) {
        const auto* source=find_region(rank[0].layout,link.output,EPYCBENCH_PACKAGE_OUTPUT);
        slot_bytes=std::max(slot_bytes,static_cast<std::size_t>(source->bytes));
        slot_alignment=std::max(slot_alignment,static_cast<std::size_t>(source->alignment));
    }
    const auto demand=estimate_stream(*plans,roles,page,*budget,slot_bytes,slot_alignment);
    if (!demand) return std::unexpected{demand.error()};
    if (!supplied) {
        const auto headroom=check_headroom(*demand);
        if (!headroom) return std::unexpected{headroom.error()};
    }
    StreamEvidence evidence{.release_ns={},.completion_ns={},.chunks_per_pass=chunks,
        .chunks=std::vector<StreamChunk>(item_count),.logical_handoff={},.error={}};
    std::vector<std::array<StreamStageState,2>> states(item_count);
    for (auto& row:evidence.chunks) {
        for (std::size_t stage=0;stage<roles.size();++stage) {
            auto& rank=row.ranks[stage];
            rank.role=roles[stage].id;
            rank.rank_count=1;
            rank.placement.requested_cpu=roles[stage].cpus.front();
            rank.placement.requested_node=memory_node(roles[stage],0U);
            rank.placement.page_bytes=page;
        }
    }
    std::array<Allocation,2> slots;
    std::latch launch{1},producer_ready{1},ready{2},release{1},finished{2},sealed{1},verified{2};
    StreamRing ring;
    bool launched=false;
    std::optional<StreamHandoffDigest> handoff_reference;
    auto prepare_stage=[&](std::size_t stage) -> std::expected<void,core::Error> {
        const auto& role=roles[stage];
        if (!supplied) {
            const auto pinned=set_current_thread_affinity(core::CpuSet{role.cpus.front()});
            if (!pinned) return pinned;
        }
        if (stage==0U) {
            std::optional<NativeResource> slot_owner;
            if (!supplied) {
                auto allocated=stream_native(role,page);
                if (!allocated) return std::unexpected{allocated.error()};
                slot_owner.emplace(std::move(*allocated));
            }
            for (auto& slot:slots) {
                const AllocationRequest request{link.output,EPYCBENCH_PACKAGE_OUTPUT,slot_bytes,slot_alignment,0U,false};
                auto allocated=slot_owner ? NativeResource::allocate(&*slot_owner,request):stream_mapping(nullptr,request);
                if (!allocated) return std::unexpected{allocated.error()};
                slot=std::move(*allocated);
                std::ranges::fill(slot.bytes,std::byte{0x5a});
            }
        } else {
            producer_ready.wait();
            if (ring.aborted()) return std::unexpected{invalid_stream("Streaming producer could not prepare its slots or references")};
        }
        // Hash the concatenated external input before release. The hash must be
        // independent of the number of chunks in the admitted partition.
        std::map<std::string, core::BinaryDigest> digests;
        std::map<std::string, std::uint64_t> sizes;
        for (std::size_t c=0;c<item_count;++c) {
            const auto input_reference=stage==0U ? std::span<const std::byte>{}:
                states[c][0].prepared->stream_reference(link.output);
            auto prepared=prepare_stream_chunk(role,plans->at(c)[stage],link,stage==1U,c,page,*budget,supplied,
                slots[c%2U],input_reference,states[c][stage],evidence.chunks[c].ranks[stage]);
            if (!prepared) return prepared;
            for (const auto& input : states[c][stage].prepared->immutable_inputs()) {
                if (stage == 1 && input.name == link.input) continue;
                const std::string name{input.name};
                if (!digests[name].append(input.bytes) ||
                    input.bytes.size() > core::Sha256Stream::maximum_input_bytes - sizes[name])
                    return std::unexpected{invalid_stream("Cannot hash external stream input")};
                sizes[name] += input.bytes.size();
            }
        }
        auto& inputs = evidence.external_inputs[stage];
        inputs = Json::object();
        for (auto& [name, hash] : digests) {
            const auto digest = hash.finish();
            if (!digest) return std::unexpected{invalid_stream("Cannot finalize external stream input hash")};
            inputs[name] = {{"bytes", sizes.at(name)}, {"sha256", digest->to_hex()}};
        }
        if (stage == 0U) {
            const auto reference = hash_stream_handoff(states, link, true);
            if (!reference) return std::unexpected{reference.error()};
            handoff_reference = *reference;
        }
        return {};
    };
    auto run_stage=[&](std::size_t stage) {
        for (std::size_t c=0;c<item_count;++c) {
            auto& state=states[c][stage];
            auto& row=evidence.chunks[c];
            if (stage==0U && c!=0U && c%chunks==0U && !ring.drain(c)) return;
            if (!ring.acquire(c,stage==1U,stage==0U ? row.producer_waited:row.consumer_waited)) return;
            if (stage==1U) {
                const auto target=state.prepared->stream_input(link.input);
                std::memcpy(target.data(),slots[c%2U].bytes.data(),target.size());
                const auto delivered=supplied ? std::expected<std::uint64_t,core::Error>{supplied->handoff_ns[c]}:phase_now({});
                if (!delivered) {
                    row.ranks[stage].error=delivered.error();
                    ring.abort();
                    return;
                }
                row.handoff_ns=*delivered;
            }
            auto& rank = row.ranks[stage];
            PackageMonitorClock monitored{state.resources.services.clock, monitor, rank.placement.requested_cpu,
                                          state.resources.services.monitoring, rank.monitoring};
            state.callback=state.prepared->invoke_stream(monitored.clock());
            if (!state.callback->completed()) {
                ring.abort();
                return;
            }
            ring.advance(c,stage==1U);
        }
    };
    auto retire_stage=[&](std::size_t stage) {
        if (stage == 1U && handoff_reference &&
            std::ranges::all_of(evidence.chunks, [](const auto& chunk) { return chunk.handoff_ns.has_value(); })) {
            const auto observed = hash_stream_handoff(states, link, false);
            if (!observed) {
                if (!evidence.error) evidence.error = observed.error();
            } else if (observed->bytes != handoff_reference->bytes) {
                if (!evidence.error) evidence.error = invalid_stream("Logical stream handoff byte count differs from reference");
            } else {
                evidence.logical_handoff = StreamEvidence::Handoff{observed->bytes, handoff_reference->sha256,
                    observed->sha256};
            }
        }
        for (std::size_t c=0;c<item_count;++c) {
            auto& row=evidence.chunks[c];
            auto& state=states[c][stage];
            auto& rank=row.ranks[stage];
            if (!state.callback) continue;
            const auto handoff=stage==0U && row.handoff_ns ?
                states[c][1].prepared->stream_input(link.input):std::span<std::byte>{};
            rank.trial=std::move(*state.prepared).retire_stream(std::move(*state.callback),
                stage==0U ? std::string_view{link.output}:std::string_view{},handoff);
            if (stage==0U && row.handoff_ns) {
                const auto origin=std::ranges::find(rank.trial->regions,link.output,&RegionEvidence::name);
                if (origin==rank.trial->regions.end() || origin->after!=origin->reference)
                    rank.error=invalid_stream("Streaming handoff bytes differ from their independent reference");
            } else if (stage==1U) {
                const auto target=std::ranges::find(rank.trial->regions,link.input,&RegionEvidence::name);
                if (target!=rank.trial->regions.end() && target->after)
                    rank.transfers.push_back({roles[0].id,link.output,link.input,target->bytes,*target->after});
            }
            if (state.resources.native) {
                auto placed=state.resources.native->observe(rank.placement,true);
                rank.placement.verified=placed.has_value();
                if (!placed) rank.error=std::move(placed.error());
            } else {
                rank.placement.verified=true;
            }
        }
    };
    auto worker=[&](std::size_t stage) {
        launch.wait();
        if (!launched) return;
        try {
            auto prepared=prepare_stage(stage);
            if (!prepared) {
                evidence.chunks.front().ranks[stage].error=prepared.error();
                ring.abort();
            }
        } catch (const std::exception&) {
            evidence.chunks.front().ranks[stage].error=invalid_stream("Streaming preparation failed");
            ring.abort();
        }
        if (stage==0U) producer_ready.count_down();
        ready.count_down();
        release.wait();
        try {
            if (!ring.aborted()) run_stage(stage);
        } catch (const std::exception&) {
            evidence.chunks.front().ranks[stage].error=invalid_stream("Streaming callback or handoff failed");
            ring.abort();
        }
        finished.count_down();
        sealed.wait();
        try {
            retire_stage(stage);
        } catch (const std::exception&) {
            evidence.chunks.front().ranks[stage].error=invalid_stream("Streaming verification could not retire");
        }
        verified.arrive_and_wait();
        for (std::size_t c=0;c<item_count;++c) {
            auto& resources=states[c][stage].resources;
            auto& rank=evidence.chunks[c].ranks[stage];
            if (resources.native) resources.native->retire_inputs(rank.placement,rank.cleanup_error);
        }
    };
    std::array<std::thread,2> workers;
    try {
        for (std::size_t stage=0;stage<workers.size();++stage) workers[stage]=std::thread(worker,stage);
        launched=true;
    } catch (const std::exception&) {
        launch.count_down();
        for (auto& thread:workers) if (thread.joinable()) thread.join();
        return std::unexpected{invalid_stream("Could not launch both streaming workers")};
    }
    launch.count_down();
    ready.wait();
    if (!ring.aborted()) {
        const auto start=supplied ? std::expected<std::uint64_t,core::Error>{supplied->release_ns}:phase_now({});
        if (start) evidence.release_ns=*start;
        else {
            evidence.error=start.error();
            ring.abort();
        }
    }
    release.count_down();
    finished.wait();
    if (evidence.release_ns) {
        const auto stop=supplied ? std::expected<std::uint64_t,core::Error>{supplied->completion_ns}:phase_now({});
        if (stop) evidence.completion_ns=*stop;
        else evidence.error=stop.error();
    }
    sealed.count_down();
    for (auto& thread:workers) thread.join();
    return evidence;
}
