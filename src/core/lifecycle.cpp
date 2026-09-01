// Satsuma Host 运行生命周期状态和原子持久化实现。
#include "satsuma/core/lifecycle.hpp"

#include <array>
#include <utility>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_contract.hpp"
#include "satsuma/core/json_io.hpp"

namespace satsuma {
namespace {

// 判断指定状态迁移是否属于已定义的生命周期图。
[[nodiscard]] bool is_allowed_transition(const RunPhase from, const RunPhase to) noexcept {
    switch (from) {
    case RunPhase::Preparing:
        return to == RunPhase::RestoringBefore || to == RunPhase::StartingVm ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::RestoringBefore:
        return to == RunPhase::StartingVm || to == RunPhase::RecoveryFailed ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::StartingVm:
        return to == RunPhase::WaitingAgent || to == RunPhase::Recovering ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::WaitingAgent:
        return to == RunPhase::Deploying || to == RunPhase::Recovering ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::Deploying:
        return to == RunPhase::Executing || to == RunPhase::RunningFinally ||
            to == RunPhase::Recovering || to == RunPhase::ManualInterventionRequired;
    case RunPhase::Executing:
        return to == RunPhase::CollectingEvidence || to == RunPhase::ManualInterventionRequired;
    case RunPhase::CollectingEvidence:
        return to == RunPhase::RunningFinally || to == RunPhase::Recovering ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::RunningFinally:
        return to == RunPhase::Recovering || to == RunPhase::Completed || to == RunPhase::Failed ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::Recovering:
        return to == RunPhase::Completed || to == RunPhase::Failed || to == RunPhase::RecoveryFailed ||
            to == RunPhase::ManualInterventionRequired;
    case RunPhase::Completed:
    case RunPhase::Failed:
    case RunPhase::RecoveryFailed:
    case RunPhase::ManualInterventionRequired:
        return false;
    }
    return false;
}

// 验证状态记录和迁移历史的内部一致性。
void validate_lifecycle_state(const RunLifecycleState& state) {
    if (state.schema_version != 1) {
        throw Error("Run lifecycle state requires schema_version 1");
    }
    validate_identifier(state.run_id, "run lifecycle run_id");
    if (state.updated_at.empty()) {
        throw Error("Run lifecycle updated_at must not be empty");
    }
    if (state.sequence != state.transitions.size()) {
        throw Error("Run lifecycle sequence does not match transition count");
    }

    RunPhase expected = RunPhase::Preparing;
    for (std::size_t index = 0; index < state.transitions.size(); ++index) {
        const RunTransition& transition = state.transitions[index];
        if (transition.sequence != index + 1 || transition.from != expected ||
            !is_allowed_transition(transition.from, transition.to) || transition.occurred_at.empty()) {
            throw Error("Run lifecycle transition history is invalid");
        }
        expected = transition.to;
    }
    if (state.phase != expected) {
        throw Error("Run lifecycle phase does not match transition history");
    }
}

}  // namespace

std::string_view run_phase_name(const RunPhase phase) {
    switch (phase) {
    case RunPhase::Preparing: return "preparing";
    case RunPhase::RestoringBefore: return "restoring_before";
    case RunPhase::StartingVm: return "starting_vm";
    case RunPhase::WaitingAgent: return "waiting_agent";
    case RunPhase::Deploying: return "deploying";
    case RunPhase::Executing: return "executing";
    case RunPhase::CollectingEvidence: return "collecting_evidence";
    case RunPhase::RunningFinally: return "running_finally";
    case RunPhase::Recovering: return "recovering";
    case RunPhase::Completed: return "completed";
    case RunPhase::Failed: return "failed";
    case RunPhase::RecoveryFailed: return "recovery_failed";
    case RunPhase::ManualInterventionRequired: return "manual_intervention_required";
    }
    throw Error("Unknown run lifecycle phase");
}

RunPhase parse_run_phase(const std::string_view name) {
    constexpr std::array phases{
        RunPhase::Preparing,
        RunPhase::RestoringBefore,
        RunPhase::StartingVm,
        RunPhase::WaitingAgent,
        RunPhase::Deploying,
        RunPhase::Executing,
        RunPhase::CollectingEvidence,
        RunPhase::RunningFinally,
        RunPhase::Recovering,
        RunPhase::Completed,
        RunPhase::Failed,
        RunPhase::RecoveryFailed,
        RunPhase::ManualInterventionRequired,
    };
    for (const RunPhase phase : phases) {
        if (run_phase_name(phase) == name) {
            return phase;
        }
    }
    throw Error("Unknown run lifecycle phase: " + std::string(name));
}

bool is_terminal_run_phase(const RunPhase phase) noexcept {
    return phase == RunPhase::Completed || phase == RunPhase::Failed ||
        phase == RunPhase::RecoveryFailed || phase == RunPhase::ManualInterventionRequired;
}

RunLifecycleState make_run_lifecycle_state(std::string run_id, std::string timestamp) {
    validate_identifier(run_id, "run lifecycle run_id");
    if (timestamp.empty()) {
        throw Error("Run lifecycle timestamp must not be empty");
    }
    RunLifecycleState state;
    state.run_id = std::move(run_id);
    state.updated_at = std::move(timestamp);
    return state;
}

void apply_run_transition(
    RunLifecycleState& state,
    const RunPhase next,
    std::string timestamp,
    std::string message) {
    validate_lifecycle_state(state);
    if (timestamp.empty()) {
        throw Error("Run lifecycle transition timestamp must not be empty");
    }
    if (!is_allowed_transition(state.phase, next)) {
        throw Error(
            "Invalid run lifecycle transition from " + std::string(run_phase_name(state.phase)) +
            " to " + std::string(run_phase_name(next)));
    }

    const RunPhase previous = state.phase;
    ++state.sequence;
    state.phase = next;
    state.updated_at = timestamp;
    state.transitions.push_back({
        state.sequence,
        previous,
        next,
        std::move(timestamp),
        std::move(message),
    });
}

void persist_run_transition(
    const std::filesystem::path& path,
    RunLifecycleState& state,
    const RunPhase next,
    std::string timestamp,
    std::string message) {
    RunLifecycleState updated = state;
    apply_run_transition(updated, next, std::move(timestamp), std::move(message));
    write_json_atomic(path, updated);
    state = std::move(updated);
}

RunLifecycleState load_run_lifecycle_state(const std::filesystem::path& path) {
    try {
        return load_json(path).get<RunLifecycleState>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid run lifecycle state: " + std::string(error.what()));
    }
}

void to_json(nlohmann::json& value, const RunLifecycleState& state) {
    value = {
        {"schema_version", state.schema_version},
        {"run_id", state.run_id},
        {"phase", run_phase_name(state.phase)},
        {"sequence", state.sequence},
        {"updated_at", state.updated_at},
        {"transitions", nlohmann::json::array()},
    };
    for (const RunTransition& transition : state.transitions) {
        value["transitions"].push_back({
            {"sequence", transition.sequence},
            {"from", run_phase_name(transition.from)},
            {"to", run_phase_name(transition.to)},
            {"occurred_at", transition.occurred_at},
            {"message", transition.message},
        });
    }
}

void from_json(const nlohmann::json& value, RunLifecycleState& state) {
    reject_unknown_fields(
        value,
        {"schema_version", "run_id", "phase", "sequence", "updated_at", "transitions"},
        "run lifecycle state");
    state.schema_version = value.value("schema_version", 0);
    state.run_id = value.at("run_id").get<std::string>();
    state.phase = parse_run_phase(value.at("phase").get<std::string>());
    state.sequence = value.at("sequence").get<std::uint64_t>();
    state.updated_at = value.at("updated_at").get<std::string>();
    state.transitions.clear();
    for (const auto& transition_value : value.at("transitions")) {
        reject_unknown_fields(
            transition_value,
            {"sequence", "from", "to", "occurred_at", "message"},
            "run lifecycle transition");
        state.transitions.push_back({
            transition_value.at("sequence").get<std::uint64_t>(),
            parse_run_phase(transition_value.at("from").get<std::string>()),
            parse_run_phase(transition_value.at("to").get<std::string>()),
            transition_value.at("occurred_at").get<std::string>(),
            transition_value.at("message").get<std::string>(),
        });
    }
    validate_lifecycle_state(state);
}

}  // namespace satsuma
