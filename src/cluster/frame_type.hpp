// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dannan/sched/work.h>

namespace macha {

// How work travels and what it is, by the laws' ranking: Dannan's, the one
// definition. FrameType is the wire encoding of a class, with read_ahead a hint
// behind foreground; WorkClass is control (law 1), foreground, the viewer
// (law 2), loader (law 3), then speculative (repair, prefetch, maintenance).
// Every subsystem that admits, paces or measures work by class reads
// work_class(), and none decides class from a frame itself.
using dannan::sched::FrameType;
using dannan::sched::frame_for;
using dannan::sched::frame_type_name;
using dannan::sched::work_class;
using dannan::sched::WorkClass;

} // namespace macha
