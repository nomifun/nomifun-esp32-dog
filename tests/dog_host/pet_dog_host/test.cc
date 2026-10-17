
#include "fake.h"
#include <filesystem>
#include <iostream>
#include "pet_dog.h"
int main(){
 const std::string a=R"({"stand":{"lf":80,"rf":85,"lb":90,"rb":95}})";
 const std::string flat=R"({"lf":170,"rf":175,"lb":5,"rb":10})";
 const std::string action=R"({"steps":[{"lf":90,"rf":90,"lb":90,"rb":90,"delay":500}]})";
 const std::string long_action=R"({"steps":[{"lf":30,"rf":60,"lb":90,"rb":120,"delay":100000}]})";
 {
 PetDog dog;
 assert(!dog.DoAction("stand"));
 assert(dog.SaveAngleSettings(a));
 assert(dog.GetAngleSettings().find("\"lf\":80")!=std::string::npos);
 assert(dog.SaveAngleSettings(flat,"sleep"));
 const auto saved=dog.GetAngleSettings();
 assert(!dog.SaveAngleSettings(R"({"stand":{"lf":181,"rf":90,"lb":90,"rb":90}})"));
 assert(!dog.SaveAngleSettings(R"({"lf":1.5,"rf":90,"lb":90,"rb":90})"));
 assert(!dog.SaveAngleSettings(R"({"lf":90,"rf":90,"lb":90})"));
 assert(!dog.SaveAngleSettings(flat+" trailing"));
 assert(!dog.SaveAngleSettings(flat+std::string(1,'\0')+"ignored"));
 assert(!dog.SaveAngleSettings(flat,"unknown"));
 assert(dog.GetAngleSettings()==saved);
 fail_commit=true; assert(!dog.SaveAngleSettings(flat,"stand"));fail_commit=false;
 assert(dog.GetAngleSettings()==saved && nvs_values["petdog/angle_settings"]==saved);
 assert(!dog.SaveCustomAction("../escape",action));
 assert(!dog.SaveCustomAction("invalid",R"({"steps":[]})"));
 assert(!dog.SaveCustomAction("invalid",R"({"steps":[{"lf":90,"rf":90,"lb":90,"rb":90,"delay":-1}]})"));
 assert(!dog.SaveCustomAction("invalid",std::string(8193,' ')));
 assert(dog.SaveCustomAction("hello",action));
 assert(dog.LoadCustomAction("hello")==action);
 assert(dog.SaveCustomAction("hello",long_action));
 assert(dog.LoadCustomAction("hello.json")==long_action);
 assert(dog.ListCustomActions()==std::vector<std::string>{"hello"});
 dog.InitializeDog(14,17,13,18);
 assert(created_tasks==2);
 assert(!dog.DoAction("missing"));assert(!dog.DoAction("walk_forward",0));assert(!dog.DoAction("turn_left",3,361));
 assert(dog.DoAction("walk_forward",100));vTaskDelay(100);dog.stop();const int stopped_writes=writes;vTaskDelay(80);assert(writes==stopped_writes);
 auto start=std::chrono::steady_clock::now(); assert(dog.ExecuteCustomAction("hello"));
 assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(1000));
 vTaskDelay(50); dog.stop(); const int after_custom=writes;vTaskDelay(60);assert(writes==after_custom);
 assert(dog.DoAction("jump"));vTaskDelay(30);assert(dog.DoAction("stand"));vTaskDelay(2000);
 assert(!dog.IsActionRunning());
 assert(duty[1] == uint32_t(80 * per_angle + LEDC_MIN_DUTY));
 assert(duty[2] == uint32_t((180 - 85) * per_angle + LEDC_MIN_DUTY));
 assert(duty[3] == uint32_t(90 * per_angle + LEDC_MIN_DUTY));
 assert(duty[0] == uint32_t(83 * per_angle + LEDC_MIN_DUTY)); // RB only: mirrored 95deg, then -2deg output trim.
 // Exercise public calibration + action paths at both RB endpoints. The PWM
 // trim must not change logical calibration or accumulate during interpolation.
 auto check_stand = [&](int rb, int expected_physical_rb) {
   const std::string angles = "{\"lf\":80,\"rf\":85,\"lb\":90,\"rb\":" + std::to_string(rb) + "}";
   assert(dog.SaveAngleSettings(angles));
   const auto untrimmed_settings = dog.GetAngleSettings();
   assert(untrimmed_settings.find("\"rb\":" + std::to_string(rb)) != std::string::npos);
   assert(dog.DoAction("stand"));
   vTaskDelay(30); // Let the action worker dequeue this command.
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
   while(dog.IsActionRunning() && std::chrono::steady_clock::now() < deadline) vTaskDelay(10);
   assert(!dog.IsActionRunning());
   assert(duty[1] == uint32_t(80 * per_angle + LEDC_MIN_DUTY));
   assert(duty[2] == uint32_t(95 * per_angle + LEDC_MIN_DUTY));
   assert(duty[3] == uint32_t(90 * per_angle + LEDC_MIN_DUTY));
   assert(duty[0] == uint32_t(expected_physical_rb * per_angle + LEDC_MIN_DUTY));
   assert(dog.GetAngleSettings() == untrimmed_settings);
   assert(nvs_values["petdog/angle_settings"] == untrimmed_settings);
 };
 check_stand(0,178);
 check_stand(180,0); // Negative physical result clamps to 0 rather than wrapping.
 check_stand(95,83);
 check_stand(95,83);
 check_stand(95,83); // Repeated commands keep a constant trim and terminate.
 assert(dog.SaveAngleSettings(a));
 assert(created_tasks==2);dog.stop();
 assert(dog.DeleteCustomAction("hello"));assert(dog.ListCustomActions().empty());
 assert(!dog.ExecuteCustomAction("missing"));
 }
 {
 PetDog loaded;loaded.InitializeDog(14,17,13,18);
 assert(loaded.GetAngleSettings()==nvs_values["petdog/angle_settings"]);
 }
 while(live_tasks>0)vTaskDelay(1);
 {
 pause_action_worker=true;
 PetDog dog;dog.InitializeDog(14,17,13,18);
 while(!action_worker_waiting.load())vTaskDelay(1);
 // Pause the worker at task entry so the queue-to-worker gap is deterministic.
 assert(dog.DoAction("sitdown"));
 assert(dog.IsActionRunning()); // Includes the pending command.
 assert(!dog.TryAutomaticPosture("stand"));
 assert(!dog.TryAutomaticPosture("sleep"));
 // Also exercise the automatic-first order from an empty queue.
 dog.stop();dog.SetFreeExplore(true);
 assert(dog.TryAutomaticPosture("sleep"));
 assert(dog.DoAction("sitdown"));
 // Regardless of which thread gets the lock first, the explicit command wins.
 for(int i=0;i<100;++i){
   dog.stop();dog.SetFreeExplore(true); // Empty queue, no posture hold.
   std::atomic<bool> start{false};
   std::thread automatic([&]{while(!start.load())std::this_thread::yield();dog.TryAutomaticPosture("sleep");});
   std::thread explicit_command([&]{while(!start.load())std::this_thread::yield();assert(dog.DoAction("squat"));});
   start=true;automatic.join();explicit_command.join();
   assert(dog.IsActionRunning());assert(!dog.TryAutomaticPosture("stand"));
 }
 dog.SetFreeExplore(false);
 pause_action_worker=false;
 int waits=0;
 auto wait_finished=[&](int timeout_ms=7000){
   ++waits;
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
   while(dog.IsActionRunning()&&std::chrono::steady_clock::now()<deadline)vTaskDelay(5);
   if(dog.IsActionRunning()){
     std::cerr<<"Timed out waiting for action at wait "<<waits<<"\n";
     std::lock_guard lock(log_mutex);
     for(size_t i=info_logs.size()>8?info_logs.size()-8:0;i<info_logs.size();++i)std::cerr<<info_logs[i]<<"\n";
   }
   assert(!dog.IsActionRunning());
 };
 wait_finished();
 assert(duty[3]==uint32_t(30*per_angle+LEDC_MIN_DUTY));
 assert(duty[0]==uint32_t(148*per_angle+LEDC_MIN_DUTY));
 // Completed explicit postures remain held through all conversation states.
 assert(!dog.TryAutomaticPosture("stand"));assert(!dog.TryAutomaticPosture("sleep"));
 for(const char* alias:{"sit","crouch","squat","坐下","蹲下"}){
   assert(dog.DoAction(alias));wait_finished();assert(!dog.TryAutomaticPosture("stand"));
 }
 assert(dog.DoAction("stand"));wait_finished();assert(!dog.TryAutomaticPosture("sleep"));
 // Windows host sleep granularity can stretch the 20ms RTOS delay slices.
 assert(dog.DoAction("lie_down"));wait_finished(15000);assert(!dog.TryAutomaticPosture("stand"));
 assert(dog.DoAction("趴下"));dog.stop();wait_finished();
 assert(!dog.TryAutomaticPosture("stand"));assert(!dog.TryAutomaticPosture("sleep"));
 // A subsequent explicit motion releases the posture hold, but automatic
 // posture changes still cannot interrupt the pending/running motion.
 size_t log_start;
 {std::lock_guard lock(log_mutex);log_start=info_logs.size();}
 assert(dog.DoAction("backward",1));assert(!dog.TryAutomaticPosture("sleep"));
 auto has_started=[&]{std::lock_guard lock(log_mutex);return std::any_of(info_logs.begin()+log_start,info_logs.end(),[](const std::string& line){return line.find("Action started #")!=std::string::npos;});};
 const auto started_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!has_started()&&std::chrono::steady_clock::now()<started_deadline)vTaskDelay(5);
 assert(has_started());assert(dog.IsActionRunning());assert(!dog.TryAutomaticPosture("stand"));
 wait_finished();
 assert(dog.TryAutomaticPosture("stand"));wait_finished();
 assert(!dog.TryAutomaticPosture("jump"));assert(!dog.TryAutomaticPosture(nullptr));
 assert(!dog.DoAction("unknown"));
 assert(dog.DoAction("walk_backward",1));dog.stop();wait_finished();
 assert(dog.DoAction("forward",1));dog.stop();wait_finished();
 assert(dog.DoAction("dance"));dog.stop();wait_finished();
 // Explicitly enabling autonomous exploration releases a held posture.
 dog.SetFreeExplore(true);assert(dog.TryAutomaticPosture("stand"));wait_finished();dog.SetFreeExplore(false);
 }
 while(live_tasks>0)vTaskDelay(1);
 {
 std::lock_guard lock(log_mutex);
 for(const char* marker:{"Action accepted #","Action started #","Action finished #","Action superseded"})
   assert(std::any_of(info_logs.begin(),info_logs.end(),[&](const std::string& line){return line.find(marker)!=std::string::npos;}));
 }
 std::cout<<"PASS: calibration validation/persistence/failure rollback, SD action CRUD, command validation, nonblocking playback, stop cancellation, replacement, bounded worker count, NVS reload, RB-only PWM trim, endpoint clamps, repeated-command nonaccumulation and worker completion\n";
 std::cout<<"PASS: pending visibility, atomic explicit/automatic arbitration (100 concurrent pairs), completed sit/stand/lie/stop hold, strict aliases, subsequent explicit motion, automatic recovery and free-explore opt-in\n";
}
