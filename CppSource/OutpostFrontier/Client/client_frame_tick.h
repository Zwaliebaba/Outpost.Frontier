// Author: Giperion.

#pragma once

// baseline per-frame tick the routine (partial).
void TickClientFrame(double DeltaSeconds);

// The render-frame bracket for the modal loops outside TickClientFrame (the NN dialogs'
// / the routine / the routine and WaitForServerResponse's multiplayer wait):
// BeginScene (with the device-recovery path); true when the frame began.
bool BeginClientRenderFrame();
// then the routine -- EndScene and present, back to back, as the modal loops and
// the menu frame issue them (only when the routine returned true).
void EndClientRenderFrame();

// The two halves on their own, for the space frame the branch, which runs the routine,
// , the routine and the camera-mode checks between them (0x0054b159 / 0x0054b21a).
void EndClientRenderScene();
void PresentClientRenderFrame();

// (cdecl, dt) -- NeuronRuntime::TextureLib slot 0x20 (0, dt), then NeuronRuntime::MatAnimLibrary
// slot 0x18 (-1, dt): the streaming-texture and material-animation tick.
void TickMaterialAnimationPlayback(float DeltaSeconds);
