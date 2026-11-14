// Actual SDK window-event -> assignment/merge -> guest gamepad state contract.
// Does not test online prediction, weapon simulation or reload animation.
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

int main(int argc,char** argv){
  using namespace rex::input;
  if(argc!=2)return 1;
  rex::InitLogging(argv[1]);
  for(auto [name,value]:{std::pair{"mnk_mode","true"},std::pair{"keybind_x","R"},std::pair{"keybind_y","F"}})
    if(!rex::cvar::SetFlagByName(name,value))return 2;
  auto driver=std::make_unique<mnk::MnkInputDriver>(nullptr,0);auto* keyboard=driver.get();
  if(driver->Setup())return 3;
  InputSystem input(nullptr);input.AddDriver(std::move(driver));input.SetDeviceAssignment(std::make_unique<SlotAssignment>(1));
  using Scope=uint32_t(*)(uint32_t);
  auto scope=reinterpret_cast<Scope>(GetProcAddress(GetModuleHandleW(L"rexruntime.dll"),"rex_ao4_stick_poll_scope_v1"));
  if(!scope)return 4;
  X_INPUT_STATE state{};
  auto read=[&](uint16_t expected,uint32_t caller=0x82F86ABC){scope(caller);return input.GetState(0,&state)==0&&(uint16_t(state.gamepad.buttons)&0xC000)==expected;};
  if(!read(0))return 5;
  unsigned checks=1;
  for(unsigned which=0;which<2;++which){
    const uint16_t bit=which?0x8000:0x4000;
    rex::ui::KeyEvent event(nullptr,which?rex::ui::VirtualKey::kF:rex::ui::VirtualKey::kR,1,false,false,false,false,false);
    for(int duration:{90,650}){
      keyboard->OnKeyDown(event);
      const auto start=std::chrono::steady_clock::now();
      do {
        // A presence probe must not release/consume a held action.
        if(!read(bit,0x83228B48)||!read(bit))return 6;
        checks+=2;
        keyboard->OnKeyDown(event); // Windows repeat does not toggle the action.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(duration));
      keyboard->OnKeyUp(event);
      if(!read(0)||!read(0,0x83228B48)||!read(0))return 7;
      checks+=3;
    }
  }
  rex::ui::KeyEvent held(nullptr,rex::ui::VirtualKey::kF,1,false,false,false,false,false);
  keyboard->OnKeyDown(held);if(!read(0x8000))return 8;
  rex::ui::UISetupEvent focus(nullptr);keyboard->OnLostFocus(focus);if(!read(0))return 9;
  keyboard->OnGotFocus(focus);if(!read(0))return 10;
  std::printf("PASS: %u state checks, 90/650ms reload+switch, repeat events, presence probes, immediate release and focus reset.\n",checks+3);
  input.Shutdown();rex::ShutdownLogging();return 0;
}
