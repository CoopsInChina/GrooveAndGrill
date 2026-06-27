#pragma once

// Show the startup logo: fade in 1 s, hold 1 s, fade out 1 s.
// done_cb (optional) is invoked from LVGL task context when the animation
// finishes — call speedo_ui_set_visible(true) or similar from it.
// Call splash_show itself with the LVGL lock held.
void splash_show(void (*done_cb)(void));
