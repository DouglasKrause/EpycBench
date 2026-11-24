#include "epycbench/kernel/task.hpp"

#include "kernel/mix64.hpp"
#include "kernel/task_domain_support.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace epycbench::kernel {
namespace {

using detail::maximum_sum_u64_elements;
using detail::sum_u64_error;
using detail::xor_u64_error;

auto read_word(std::span<const std::byte> bytes, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t word{};
    for(unsigned byte{};byte<8U;++byte)
        word|=std::uint64_t{std::to_integer<unsigned char>(bytes[offset+byte])}<<(byte*8U);
    return word;
}
void write_word(std::span<std::byte> bytes,std::size_t offset,std::uint64_t word) noexcept {
    for(unsigned byte{};byte<8U;++byte)
        bytes[offset+byte]=static_cast<std::byte>((word>>(byte*8U))&0xffU);
}
auto xor_expected_word(const XorU64Config& configuration,std::uint64_t index) noexcept -> std::uint64_t {
    return detail::mix64_v1(configuration.input_seed()+index)^configuration.xor_mask();
}

} // namespace

auto ElementCount::create(std::uint64_t value) -> std::expected<ElementCount, core::Error> {
    if (value == 0U)
        return std::unexpected{sum_u64_error("zero_element_count", "element_count")};
    return ElementCount{value};
}

auto TaskRepetitionCount::create(std::uint64_t value)
    -> std::expected<TaskRepetitionCount, core::Error> {
    if (value == 0U)
        return std::unexpected{sum_u64_error("zero_repetitions")};
    return TaskRepetitionCount{value};
}

auto SumU64Config::create(std::uint64_t element_count, std::uint64_t input_seed)
    -> std::expected<SumU64Config, core::Error> {
    auto count = ElementCount::create(element_count);
    if (!count)
        return std::unexpected{std::move(count.error())};
    if (element_count > maximum_sum_u64_elements())
        return std::unexpected{sum_u64_error("input_size_overflow", "element_count")};
    return SumU64Config{*count, input_seed, core::ByteCount::from_base_units(element_count * 8U)};
}

auto SumU64Config::native_bytes() const noexcept -> std::size_t {
    return static_cast<std::size_t>(logical_bytes_.value());
}

auto SumU64Config::maximum_repetitions() const noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() / element_count_.value();
}

auto SumU64Work::create(const SumU64Config& configuration, TaskRepetitionCount repetitions)
    -> std::expected<SumU64Work, core::Error> {
    return create(configuration.element_count(),repetitions);
}

auto SumU64Work::create(ElementCount count, TaskRepetitionCount repetitions)
    -> std::expected<SumU64Work, core::Error> {
    if (repetitions.value() > std::numeric_limits<std::uint64_t>::max()/count.value())
        return std::unexpected{sum_u64_error("processed_elements_overflow")};
    auto elements = ElementCount::create(count.value() * repetitions.value());
    if (!elements)
        return std::unexpected{std::move(elements.error())};
    return SumU64Work{repetitions, *elements};
}

auto initialize_sum_u64_input(const SumU64Config& configuration, std::span<std::byte> storage)
    -> std::expected<void, core::Error> {
    if (storage.size() < configuration.native_bytes())
        return std::unexpected{sum_u64_error("storage_too_small")};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        const auto word = detail::mix64_v1(configuration.input_seed() + i);
        const auto offset = static_cast<std::size_t>(i * 8U);
        for (unsigned byte{}; byte < 8U; ++byte)
            storage[offset + byte] = static_cast<std::byte>((word >> (byte * 8U)) & 0xffU);
    }
    return {};
}

auto prepare_sum_u64_reference(const SumU64Config& configuration,
                               std::span<const std::byte> storage)
    -> std::expected<SumU64Reference, core::Error> {
    if (storage.size() < configuration.native_bytes())
        return std::unexpected{sum_u64_error("storage_too_small")};
    std::uint64_t answer{};
    for (std::uint64_t i{}; i < configuration.element_count().value(); ++i) {
        // Regenerate the independent host expectation; never call a timed module.
        const auto expected = detail::mix64_v1(configuration.input_seed() + i);
        const auto offset = static_cast<std::size_t>(i * 8U);
        std::uint64_t actual{};
        for (unsigned byte{}; byte < 8U; ++byte)
            actual |= std::uint64_t{std::to_integer<unsigned char>(storage[offset + byte])}
                      << (byte * 8U);
        if (actual != expected)
            return std::unexpected{sum_u64_error("input_content_mismatch")};
        answer += expected;
    }
    return SumU64Reference{configuration, answer,
                           core::sha256(storage.first(configuration.native_bytes()))};
}

auto replay_sum_u64_reference(const SumU64Config& configuration, core::ByteCount maximum_generated_bytes)
    -> std::expected<SumU64Reference, core::Error> {
    if (configuration.logical_bytes() > maximum_generated_bytes ||
        configuration.logical_bytes().value() > core::Sha256Stream::maximum_input_bytes)
        return std::unexpected{sum_u64_error("reference_capacity")};

    core::Sha256Stream stream;
    constexpr auto chunk_bytes = 4096U;
    std::array<std::byte, chunk_bytes> chunk;
    constexpr std::uint64_t elements_per_chunk = chunk_bytes / 8U;
    const auto elements = configuration.element_count().value();
    std::uint64_t answer{};
    for (std::uint64_t first{}; first < elements;) {
        const auto count = std::min(elements_per_chunk, elements - first);
        for (std::uint64_t index{}; index < count; ++index) {
            const auto word = detail::mix64_v1(configuration.input_seed() + first + index);
            write_word(chunk, static_cast<std::size_t>(index * 8U), word);
            answer += word;
        }
        // The total logical length was checked before the first chunk.
        static_cast<void>(stream.append(std::span{chunk}.first(static_cast<std::size_t>(count * 8U))));
        first += count;
    }
    return SumU64Reference{configuration, answer, stream.digest()};
}

auto XorU64Config::create(std::uint64_t count,std::uint64_t seed,std::uint64_t mask)
    -> std::expected<XorU64Config,core::Error> {
    if(!count) return std::unexpected{xor_u64_error("zero_element_count","element_count")};
    // Two distinct allocations must have a representable combined logical size.
    // Runtime separately checks page rounding and its combined resource budget.
    if(count>maximum_sum_u64_elements()/2U)
        return std::unexpected{xor_u64_error("combined_storage_overflow","element_count")};
    auto input=SumU64Config::create(count,seed);
    if(!input) return std::unexpected{std::move(input.error())};
    return XorU64Config{*input,mask};
}

auto prepare_xor_u64_reference(const XorU64Config& configuration,
    std::span<const std::byte> input,std::span<std::byte> output)
    -> std::expected<XorU64Reference,core::Error> {
    if(input.size()<configuration.native_bytes()||output.size()<configuration.native_bytes())
        return std::unexpected{xor_u64_error("storage_too_small")};
    const std::less<const std::byte*> less;
    if(less(input.data(),output.data()+output.size())&&less(output.data(),input.data()+input.size()))
        return std::unexpected{xor_u64_error("overlapping_storage")};
    auto source=prepare_sum_u64_reference(configuration.input_configuration(),input);
    if(!source) return std::unexpected{xor_u64_error("input_content_mismatch")};
    std::uint64_t answer{};
    for(std::uint64_t i{};i<configuration.element_count().value();++i) {
        const auto expected=xor_expected_word(configuration,i);
        write_word(output,static_cast<std::size_t>(i*8U),expected);
        answer+=expected;
    }
    const auto padding=output.subspan(configuration.native_bytes());
    std::fill(padding.begin(),padding.end(),xor_u64_padding_sentinel);
    return XorU64Reference{configuration,source->input_digest(),
        core::sha256(output.first(configuration.native_bytes())),answer};
}

auto reset_xor_u64_output(const XorU64Reference& reference,std::span<std::byte> output)
    -> std::expected<void,core::Error> {
    const auto& configuration=reference.configuration();
    if(output.size()<configuration.native_bytes())
        return std::unexpected{xor_u64_error("storage_too_small")};
    for(std::uint64_t i{};i<configuration.element_count().value();++i)
        write_word(output,static_cast<std::size_t>(i*8U),~xor_expected_word(configuration,i));
    const auto padding=output.subspan(configuration.native_bytes());
    std::fill(padding.begin(),padding.end(),xor_u64_padding_sentinel);
    return {};
}

auto verify_xor_u64_output(const XorU64Reference& reference,std::span<const std::byte> output)
    -> std::expected<XorU64Verification,core::Error> {
    const auto& configuration=reference.configuration();
    if(output.size()<configuration.native_bytes())
        return std::unexpected{xor_u64_error("storage_too_small")};
    bool content_matches=true;
    for(std::uint64_t i{};i<configuration.element_count().value();++i)
        content_matches=(read_word(output,static_cast<std::size_t>(i*8U))==xor_expected_word(configuration,i))&&content_matches;
    const auto padding=output.subspan(configuration.native_bytes());
    const auto padding_matches=std::ranges::all_of(padding,[](std::byte value){return value==xor_u64_padding_sentinel;});
    return XorU64Verification{configuration.element_count(),reference.expected_output_digest(),
        core::sha256(output.first(configuration.native_bytes())),content_matches,padding_matches};
}

} // namespace epycbench::kernel
