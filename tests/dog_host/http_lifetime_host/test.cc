#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "http_client.h"
#include "web_socket.h"
#include "network_interface.h"
#include "esp_tcp.h"
#include "esp_ssl.h"
#include "esp_tls.h"
#include <sys/socket.h>
using namespace std::chrono_literals;
static std::atomic<int> deleted_events{0}, bad_event_access{0}, task_count{0};
static std::atomic<bool> fail_task{false};
struct HostEvent { std::mutex m; std::condition_variable cv; EventBits_t bits=0; bool deleted=false; };
EventGroupHandle_t xEventGroupCreate(){ return new HostEvent; }
void vEventGroupDelete(EventGroupHandle_t e){ std::lock_guard l(e->m); e->deleted=true; ++deleted_events; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t e,EventBits_t b){ std::lock_guard l(e->m); if(e->deleted) ++bad_event_access; e->bits|=b; e->cv.notify_all(); return e->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t e,EventBits_t b){ std::lock_guard l(e->m); if(e->deleted) ++bad_event_access; auto old=e->bits; e->bits&=~b; return old; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e,EventBits_t b,int clear,int all,TickType_t timeout){ std::unique_lock l(e->m); if(e->deleted) ++bad_event_access; auto ready=[&]{return all?(e->bits&b)==b:bool(e->bits&b);}; if(timeout==portMAX_DELAY)e->cv.wait(l,ready); else e->cv.wait_for(l,std::chrono::milliseconds(timeout),ready); auto r=e->bits; if(clear && ready())e->bits&=~b; return r; }
struct HostTask {};
thread_local TaskHandle_t current_task=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,TaskHandle_t* out){ if(fail_task.exchange(false))return 0; *out=new HostTask; auto task=*out; ++task_count; std::thread([=]{current_task=task; fn(arg); --task_count;}).detach(); return pdPASS; }
TaskHandle_t xTaskGetCurrentTaskHandle(){return current_task;}
void vTaskDelete(TaskHandle_t){}
struct SocketState { std::mutex m; std::condition_variable cv; std::deque<std::string> data; bool eof=false; int closes=0; int shutdowns=0; int reads=0; int tls_destroys=0; };
static std::mutex sockets_mutex;
static std::map<int,std::shared_ptr<SocketState>> sockets;
static int last_fd=10;
static std::string auto_response;
std::shared_ptr<SocketState> sock(int fd){std::lock_guard l(sockets_mutex); return sockets.at(fd);}
int host_socket(int,int,int){std::lock_guard l(sockets_mutex); sockets[++last_fd]=std::make_shared<SocketState>();return last_fd;}
int host_connect(int,const sockaddr*,int){return 0;}
int host_recv(int fd,void* p,size_t n,int){auto s=sock(fd);std::unique_lock l(s->m);++s->reads;s->cv.notify_all();s->cv.wait(l,[&]{return s->eof||!s->data.empty();});if(s->data.empty())return 0;auto d=s->data.front();s->data.pop_front();auto z=std::min(n,d.size());memcpy(p,d.data(),z);if(z<d.size())s->data.push_front(d.substr(z));return int(z);}
void push(int fd,const std::string& data,bool eof=false){auto s=sock(fd);std::lock_guard l(s->m);if(!data.empty())s->data.push_back(data);s->eof=eof;s->cv.notify_all();}
int host_send(int fd,const void* p,size_t n,int){std::string data((const char*)p,n);if(!auto_response.empty()) {push(fd,auto_response);auto_response.clear();} return int(n);}
int host_shutdown(int fd,int){auto s=sock(fd);std::lock_guard l(s->m);++s->shutdowns;s->eof=true;s->cv.notify_all();return 0;}
int host_close(int fd){auto s=sock(fd);std::lock_guard l(s->m);++s->closes;s->eof=true;s->cv.notify_all();return 0;}
esp_tls_t* esp_tls_init(){return new esp_tls{-1};}
int esp_tls_conn_new_sync(const char*,size_t,int,const esp_tls_cfg_t*,esp_tls_t* t){t->fd=host_socket(0,0,0);return 1;}
int esp_tls_get_error_handle(esp_tls_t*,esp_tls_error_handle_t*){return 0;}
int esp_tls_get_and_clear_last_error(esp_tls_error_handle_t,int*,int*){return 0;}
int esp_tls_get_conn_sockfd(esp_tls_t* t,int* out){*out=t->fd;return 0;}
void esp_tls_conn_destroy(esp_tls_t* t){auto s=sock(t->fd);{std::lock_guard l(s->m);++s->tls_destroys;}host_close(t->fd);delete t;}
int esp_tls_conn_write(esp_tls_t* t,const void* p,size_t n){return host_send(t->fd,p,n,0);}
int esp_tls_conn_read(esp_tls_t* t,void* p,size_t n){return host_recv(t->fd,p,n,0);}
#undef connect
#undef close
#undef send
#undef recv
#undef socket
#undef shutdown
struct Gate {std::mutex m;std::condition_variable cv;bool entered=false,released=false;void park(){std::unique_lock l(m);entered=true;cv.notify_all();cv.wait(l,[&]{return released;});}void await(){std::unique_lock l(m);if(!cv.wait_for(l,2s,[&]{return entered;}))throw std::runtime_error("gate timeout");}void release(){std::lock_guard l(m);released=true;cv.notify_all();}};
static int checks=0,failures=0;
void check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAIL "<<what<<"\n";}}
void drain(){for(int i=0;i<1000 && task_count;i++)std::this_thread::sleep_for(1ms);check(task_count==0,"all receiver tasks drained");}
template<class T>void passive_join(){T tcp;Gate gate;int callbacks=0;tcp.OnDisconnected([&]{++callbacks;gate.park();});check(tcp.Connect("host",80),"passive connect");int fd=last_fd;push(fd,"",true);gate.await();auto join=std::async(std::launch::async,[&]{tcp.Disconnect();});bool blocked=join.wait_for(10ms)==std::future_status::timeout;gate.release();join.get();check(blocked,"Disconnect joins already-disconnected callback");check(callbacks==1,"one passive notification");check(sock(fd)->closes==1,"passive descriptor closed once");}
template<class T>void active_close(){T tcp;int calls=0;tcp.OnDisconnected([&]{++calls;tcp.Disconnect();});check(tcp.Connect("host",80),"active connect");int fd=last_fd;tcp.Disconnect();tcp.Disconnect();check(calls==1,"active callback once and self-close safe");check(sock(fd)->closes==1,"active descriptor closed once");}
template<class T>void task_failure(){T tcp;int calls=0;tcp.OnDisconnected([&]{++calls;});fail_task=true;check(!tcp.Connect("host",80),"task creation failure returned");int fd=last_fd;check(!tcp.connected(),"failed task disconnected");tcp.Disconnect();check(calls==0,"no callback without receiver");check(sock(fd)->closes==1,"task failure releases socket once");}
class RealNetwork:public NetworkInterface {public:std::unique_ptr<Tcp>CreateTcp(int)override{return std::make_unique<EspTcp>();}std::unique_ptr<Tcp>CreateSsl(int)override{return std::make_unique<EspSsl>();}};
class ParkedTcp:public Tcp{public:bool handshake=false;Gate gate;std::thread thread;std::atomic<bool> running{false};~ParkedTcp(){Disconnect();}bool Connect(const std::string&,int)override{connected_=true;return true;}int Send(const std::string& data)override{if(handshake && data.find("GET ")==0)stream_callback_("HTTP/1.1 101 Switching Protocols\r\n\r\n");return 1;}int GetLastError()override{return 0;}void Disconnect()override{connected_=false;if(thread.joinable())thread.join();}void remote(const std::string& response){running=true;thread=std::thread([&,response]{stream_callback_(response);connected_=false;disconnect_callback_();gate.park();running=false;});}};
class ParkedNetwork:public NetworkInterface{public:ParkedTcp* last=nullptr;bool websocket=false;std::unique_ptr<Tcp>CreateTcp(int)override{auto p=std::make_unique<ParkedTcp>();p->handshake=websocket;last=p.get();return p;}std::unique_ptr<Tcp>CreateSsl(int i)override{return CreateTcp(i);}};
void http_owner(){ParkedNetwork n;auto http=std::make_unique<HttpClient>(&n);http->SetTimeout(100);check(http->Open("GET","http://host/"),"HTTP owner open");auto t=n.last;t->remote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");t->gate.await();check(http->ReadAll()=="OK","HTTP complete response");int events_before=deleted_events;auto dtor=std::async(std::launch::async,[&]{http.reset();});bool blocked=dtor.wait_for(10ms)==std::future_status::timeout;bool alive=deleted_events==events_before;t->gate.release();dtor.get();check(blocked,"HTTP destructor drains callback");check(alive,"HTTP resources alive until receiver completes");}
void http_reconnect(){ParkedNetwork n;HttpClient h(&n);h.SetTimeout(100);check(h.Open("GET","http://host/"),"HTTP first open");auto old=n.last;old->remote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");old->gate.await();auto reopen=std::async(std::launch::async,[&]{return h.Open("GET","http://next/");});bool blocked=reopen.wait_for(10ms)==std::future_status::timeout;old->gate.release();check(reopen.get(),"HTTP reopen successful");check(blocked,"HTTP reconnect drains old callbacks");}
void http_partial_and_backpressure(){RealNetwork n;{HttpClient h(&n);h.SetTimeout(100);check(h.Open("GET","http://host/"),"HTTP partial open");push(last_fd,"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc",true);check(h.GetStatusCode()==200,"partial headers readable");std::this_thread::sleep_for(5ms);check(h.ReadAll().empty(),"incomplete body rejected");h.Close();}{HttpClient h(&n);check(h.Open("GET","http://host/"),"HTTP backpressure open");int fd=last_fd;push(fd,"HTTP/1.1 200 OK\r\nContent-Length: 12000\r\n\r\n"+std::string(12000,'x'));std::this_thread::sleep_for(10ms);auto close=std::async(std::launch::async,[&]{h.Close();});check(close.wait_for(2s)==std::future_status::ready,"Close wakes blocked body producer before join");close.get();}}
void websocket_owner(){ParkedNetwork n;n.websocket=true;auto ws=std::make_unique<WebSocket>(&n,0);check(ws->Connect("ws://host/"),"WS parked owner connect");auto old=n.last;old->remote(std::string("\x88\0",2));old->gate.await();int events_before=deleted_events;auto dtor=std::async(std::launch::async,[&]{ws.reset();});bool blocked=dtor.wait_for(10ms)==std::future_status::timeout;bool alive=deleted_events==events_before;old->gate.release();dtor.get();check(blocked,"WS destructor drains peer-close callback");check(alive,"WS event survives pending receiver");}
template<class T>void stream_self_close(){T tcp;Gate gate;int calls=0;tcp.OnStream([&](const std::string&){tcp.Disconnect();});tcp.OnDisconnected([&]{++calls;gate.park();});check(tcp.Connect("host",80),"stream self-close connect");int fd=last_fd;push(fd,"x");gate.await();auto join=std::async(std::launch::async,[&]{tcp.Disconnect();});bool blocked=join.wait_for(10ms)==std::future_status::timeout;gate.release();join.get();check(blocked,"owner joins after stream callback self-close");check(calls==1,"self-close notifies once");check(sock(fd)->closes==1,"self-close releases socket once");}
void websocket_lifecycle(){RealNetwork n;auto_response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n";WebSocket ws(&n,0);check(ws.Connect("ws://host/"),"fast WS handshake not lost");int fd=last_fd;push(fd,std::string("\x89\x02hi",4));std::this_thread::sleep_for(2ms);auto_response="HTTP/1.1 101 Switching Protocols\r\n\r\n";check(ws.Connect("ws://next/"),"WS reconnect resets handshake");check(sock(fd)->closes==1,"WS reconnect drains old socket");}
int main(int argc,char** argv){bool baseline=argc>1 && std::string(argv[1])=="--baseline";try{http_owner();http_reconnect();passive_join<EspTcp>();if(!baseline){passive_join<EspSsl>();active_close<EspTcp>();active_close<EspSsl>();task_failure<EspTcp>();task_failure<EspSsl>();websocket_owner();stream_self_close<EspTcp>();stream_self_close<EspSsl>();for(int i=0;i<25;i++)http_partial_and_backpressure();websocket_lifecycle();for(int i=0;i<100;i++){active_close<EspTcp>();active_close<EspSsl>();}drain();check(bad_event_access==0,"no event group touched after destruction");}}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}std::cout<<"checks="<<checks<<" failures="<<failures<<"\n";return failures?1:0;}
