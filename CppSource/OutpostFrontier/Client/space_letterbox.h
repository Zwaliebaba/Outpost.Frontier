// Author: Giperion.

#pragma once

// The cinematic letterbox: the black bars that slide in when a SceneScript script takes the camera
// (ScriptCamera's activate slot is one of its two callers) and slide out again when it gives it
// back. It is the second of the two full-screen effects in the client, and like FadeToBlack it
// is a handful of .data globals rather than an object.

// What the bars actually are: the camera's VIEWPORT is narrowed vertically until the visible
// rectangle has a 1.85 aspect -- the widescreen cinema ratio, held as a literal float at
// .rdata 0x005c9020. Nothing is drawn; the letterbox simply stops the renderer from using the
// top and bottom of the surface, and whatever was already there stays black.

// baseline's globals:

// Amount     0 = no bars, 1 = full 1.85 bars. Interpolated.
// Target     where Amount is heading
// Speed      Amount units per second
// bAnimating whether Amount is still moving
// LastCamera the camera the current bars were applied to

// The LastCamera field is what makes the effect survive a camera switch: the update reapplies
// the viewport whenever the camera it is handed differs from the one it saw last, even if the
// amount has not moved at all.

struct FSpaceCamera;
class FRoomCamera;

// . Aim the bars at Target, moving at Speed per second. ScriptCamera calls this with
// (1.0f, 3.0f) when a script takes over and (0.0f, 3.0f) when it lets go, so the bars take a
// third of a second either way.

// If the screen is already covered by the fade (IsFadeOpaque), the amount JUMPS to the target
// instead of animating -- there is nothing to see, so there is no reason to spend the third of
// a second. baseline checks this first, before storing anything else.
void SetLetterboxTarget(float Target, float Speed);

// . Forgets which camera the bars were last applied to, which forces the next update
// to reapply them. FGameCamera's slot 0x5c calls this straight after it resets the viewport,
// because that reset has just wiped the bars out of the camera.
void ForgetLetterboxCamera();

// / the routine / the routine.
bool IsLetterboxVisible();          // strictly positive amount
float GetLetterboxAmount();
float GetLetterboxTarget();

// . Advances the amount and, when anything changed, recomputes the camera's
// viewport. Called once a frame from FGameCamera's update slot (0x0051b4b0) with the capped
// frame time. A null camera advances the amount and nothing more.
void UpdateLetterbox(FSpaceCamera* Cam, float DeltaSeconds);

// . The room camera's twin of the above: the same amount step and bar geometry, but
// the room camera (an FRoomCamera, room runtime +0x500) has no viewport setter, so its viewport,
// half extents, tangents, half-angles, aspect and side planes are rebuilt inline from its own
// slot-6 half-angle. Callers: the Gameflow tick (0x00558b66) with the frame time, the Gameflow
// letterbox reset the routine (0x00558d74) with 0, and the menu's intro tick (0x0057426c).
// A null camera advances the amount and nothing more. Shares the field with UpdateLetterbox.
void UpdateRoomLetterbox(FRoomCamera* Cam, float DeltaSeconds);

// The bar height in pixels for a given amount, on the current surface. Exposed because it is the
// whole of the geometry and is worth being able to check on its own:

//   bars = floor((Height - Width / ((1.85 - Width/Height) * Amount + Width/Height)) * 0.5 + 0.5)

// At Amount 0 the divisor is the surface's own aspect, the division gives the height back and
// the bars are zero. At Amount 1 the divisor is 1.85 and what is left is the 1.85 rectangle,
// centred. baseline computes this inline at 0x0041a4e9; it is a function here so the harness can
// pin it down without driving a camera.
int GetLetterboxBarHeight(float Amount);
