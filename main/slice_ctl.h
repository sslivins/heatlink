/// @file slice_ctl.h
/// @brief Firmware glue for group time-slicing (#133, slice::Node).
///
/// Reads the replicated slice config, collects every member's slice view
/// (self live, peers from the signed group poll), ticks the Node ~1 Hz from
/// the CN105 task and turns the resulting directive into a park hold
/// (park_ctl::set_slice). While slicing is on, AUTO is not allowed: a unit put
/// into AUTO (e.g. by the IR remote) is returned to its previous mode.

#pragma once

#include "slice_logic.h"
#include "cn105_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slice_ctl {

struct Hooks {
    std::function<cn105::Settings()>              settings;  ///< desired (park overlay)
    std::function<cn105::Status()>                status;
    std::function<void(const std::string& mode)>  set_mode;  ///< physical mode command
};

/// Load the persisted term. Call before the CN105 task.
void init(const Hooks& hooks);

/// Advance; call ~1 Hz from the CN105 task after park_ctl::tick().
void tick();

/// Wrap a user/HA power or mode command in one of these. The constructor
/// blocks tick() and, if the new desire is off-slice, holds the head before
/// the command is recorded; the destructor applies the directive for the new
/// desire (possibly releasing the hold) only after the new mode is recorded,
/// so a head never runs the wrong mode, even briefly.
class Command {
public:
    Command(bool power_on, const std::string& mode);
    ~Command();
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
};

/// Slicing is enabled for this head's group.
bool enabled();

/// What a peer published in its signed snapshot.
struct PeerObs {
    bool         capable = false;
    bool         has_state = false;
    slice::State state;
    bool         satisfied = false;
    slice::Need  need = slice::Need::Yes;  ///< absent (older firmware): Yes
    std::string  power, mode, sub_mode;
};
void observe_peer(const std::string& uid, const PeerObs& obs);

/// Fields this head publishes in its snapshot.
struct Publish {
    bool         has_state = false;
    slice::State state;
    bool         satisfied = false;
    slice::Need  need = slice::Need::Yes;
};
Publish publish();

struct MemberStatus {
    std::string uid;
    bool        fresh = false;
    bool        capable = false;
    slice::Mode wants = slice::Mode::None;
    slice::Need need = slice::Need::Yes;
    bool        satisfied = false;  ///< parked at setpoint
    slice::Directive directive = slice::Directive::Free;
};

struct Status {
    bool          in_group = false;
    slice::Config cfg;
    bool          all_capable = false;  ///< every member is fresh and slice-capable
    bool          have_state = false;
    bool          is_coordinator = false;
    std::string   coordinator;
    slice::State  state;
    uint32_t      elapsed_s = 0;
    std::vector<MemberStatus> members;  ///< self first
};
Status status();

}  // namespace slice_ctl
