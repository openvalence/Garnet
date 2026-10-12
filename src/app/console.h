// The USB console: one reader task. "@V" lines are tether frames and go
// straight to the TetherLink queue; every other line is a command, queued for
// the UI task (commands touch the Session, which is UI-task only).
#pragma once

#include <cstddef>

namespace garnet {
class TetherLink;
}

namespace garnet::console {

inline constexpr size_t kLineMax = 1024;

void start(TetherLink& tether);
// UI task: copies the next queued command into `out`; false when none.
bool next(char* out);

}  // namespace garnet::console
