#include "parameters.hpp"

#include <algorithm>
#include <atomic>

namespace {
auto execute(const EpycBenchPackageCallV1* call, EpycBenchPackageWorkV1* work) noexcept -> int {
    if (call->input_count != 1 || call->output_count != 1 ||
        call->inputs[0].bytes != call->outputs[0].bytes) return 1;
    const auto& request = *call->task;
    const auto scale = byte_transform::parameter(request, "scale").value.float_value;
    const auto bias = static_cast<int>(byte_transform::parameter(request, "bias").value.signed_value);
    const auto invert = byte_transform::parameter(request, "invert").value.bool_value != 0;
    const auto transform = [scale, bias, invert](unsigned char byte) {
        const auto scaled = static_cast<int>(static_cast<double>(byte) * scale) + bias;
        return static_cast<unsigned char>(static_cast<unsigned char>(scaled) ^ (invert ? 255 : 0));
    };
    const auto count = call->outputs[0].bytes;
    const auto* source = call->inputs[0].data;
    auto* target = call->outputs[0].data;
#ifdef EXAMPLE_BLOCKED
    const auto* parameter = byte_transform::named(call->tuning, call->tuning_count, "block_size");
    if (!parameter || call->scratch_count != 1) return 1;
    const auto block_size = parameter->value.unsigned_value;
    if (block_size == 0 || block_size > call->scratch[0].bytes) return 1;
    auto* block = call->scratch[0].data;
#endif
    for (std::uint64_t repetition = 0; repetition < request.repetitions; ++repetition) {
#ifdef EXAMPLE_BLOCKED
        for (std::uint64_t begin = 0; begin < count;) {
            const auto size = std::min(block_size, count - begin);
            std::transform(source + begin, source + begin + size, block, transform);
            std::copy_n(block, size, target + begin);
            begin += size;
        }
#else
        std::transform(source, source + count, target, transform);
#endif
        // Keep each completed repetition observable to the compiler.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        ++work->repetitions;
        work->work_units += count;
    }
    work->output_count = count;
    return 0;
}

constexpr EpycBenchPackageImplementationV1 implementation{EPYCBENCH_PACKAGE_ABI_VERSION,
    sizeof(EpycBenchPackageImplementationV1), execute};
} // namespace

extern "C" auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    return &implementation;
}
