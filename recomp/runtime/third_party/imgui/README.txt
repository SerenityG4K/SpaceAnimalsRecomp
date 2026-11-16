Dear ImGui v1.92.9b (https://github.com/ocornut/imgui, tag v1.92.9b), MIT license: LICENSE.txt.
Archive https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  SHA-256 21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
Unmodified. Kept: the core (imgui*.cpp, imgui*.h, imstb_*.h, imconfig.h) and the Vulkan renderer backend
(backends/imgui_impl_vulkan.*). Left out: the demo, the examples, the docs, the other backends and misc/.
Its settings are not edited into imconfig.h: they are in recomp/runtime/os/menu_imconfig.h, given by recomp/link.py
(-DIMGUI_USER_CONFIG). Compiled with the runtime by recomp/link.py.
Used by recomp/runtime/os/menu.cpp and menu_vk.cpp: the in-game menu (F1) and its FPS counter, drawn over the window's
image. Dear ImGui's own widgets and input are not used: the menu draws on its draw lists and reads its own keys,
controller and mouse.
