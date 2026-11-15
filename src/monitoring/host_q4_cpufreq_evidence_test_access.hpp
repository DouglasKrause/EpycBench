#pragma once
#include <string_view>
namespace epycbench::monitoring::qualification::detail {
void monitoring_q4_evidence_builder_test_fault(bool) noexcept;
void monitoring_q4_evidence_json_test_fault(unsigned char) noexcept;
bool validate_monitoring_q4_resolved_schema_instance(std::string_view,
                                                     std::string_view) noexcept;
}
