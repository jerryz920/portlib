
#include "libport.h"
#include "port-syscall.h"
#include "metadata.h"
#include "log.h"
#include "core_manager.h"
#include "image.h"
#include "principal.h"
#include "accessor_object.h"
#include "port_manager.h"
#include "utils.h"

#include <unordered_map>
#include <sys/types.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
using std::unordered_map;


/// Singleton instance of library storage

namespace {
std::string get_myip() {

  struct ifaddrs *addrs;
  if (getifaddrs(&addrs)) {
    throw std::runtime_error(strerror(errno));
  }

  for (struct ifaddrs *cur = addrs; cur != NULL; cur = cur->ifa_next) {
    /// filter the loopback interface
    if (strcmp(cur->ifa_name, "lo") == 0) {
      continue;
    }
    /// We don't use IPv6 at the moment
    if (cur->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    /// format IP
    char ipbuf[INET_ADDRSTRLEN + 1];
    if (!inet_ntop(AF_INET, &((struct sockaddr_in*)cur->ifa_addr)->sin_addr,
        ipbuf, INET_ADDRSTRLEN)) {
      return std::string(ipbuf);
    } else {
      freeifaddrs(addrs);
      throw std::runtime_error(strerror(errno));
    }
  }
  freeifaddrs(addrs);
  return "";
}

std::string format_id(const std::string& ip, int lo, int hi) {
  std::stringstream ss;
  ss << ip << ":" << lo << "-" << hi;
  return ss.str();
}

std::string format_principal_id(uint64_t pid) {
  return latte::utils::itoa<uint64_t, latte::utils::HexType>(pid);
}

std::string format_system_time() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  /// ctime string has exact 26 chars, 50 is enough.
  std::string result(50, ' ');
  ctime_r(&t, &result[0]);
  return result;
}

std::string check_proc(uint64_t pid) {
  std::stringstream exec_path_buf;
  exec_path_buf << "/proc/" << pid << "/exe";
  struct stat s;

  std::string exec_path = exec_path_buf.str();

  if (lstat(exec_path.c_str(), &s) < 0) {
    latte::log_err("diagnose pid %llu, lstat: %s", pid, strerror(errno));
    return "";
  }

  char *execbuf = (char*) malloc(s.st_size + 1);
  if (!execbuf) {
    latte::log_err("diagnose pid %llu, malloc: %s", pid, strerror(errno));
    return "";
  }
  int linksize = readlink(exec_path.c_str(), execbuf, s.st_size + 1);
  if (linksize < 0) {
    latte::log_err("diagnose pid %llu, readlink: %s", pid, strerror(errno));
    free(execbuf);
    return "";
  }
  if (linksize > s.st_size) {
    latte::log_err("diagnose pid %llu, link size changed between stat", pid);
    free(execbuf);
    return "";
  }
  execbuf[linksize] = '\0';
  auto result = std::string(execbuf, execbuf + linksize);
  free(execbuf);
  return result;
}

std::string read_file(const std::string& fpath) {
  struct stat s;
  /// We use c style but we should be good
  if (stat(fpath.c_str(), &s) < 0) {
    latte::log_err("read file %s, lstat: %s", fpath.c_str(), strerror(errno));
    return "";
  }
  std::FILE* f = fopen(fpath.c_str(), "r");
  if (f) {
    std::string result(s.st_size, ' ');
    std::fread(&result[0], s.st_size, 1, f);
    std::fclose(f);
    return result;
  } else {
    latte::log_err("read file %s, open: %s", fpath.c_str(), strerror(errno));
    return "";
  }
}

std::string make_default_restore_path(pid_t p) {
  std::stringstream ss;
  ss << "/var/run/libport/conf-" << p << ".json";
  return ss.str();
}

template <typename O, class... Args>
inline std::unique_ptr<O> make_unique(Args&&... args) {
  return std::unique_ptr<O>(new O(std::forward<Args>(args)...));
}

template <typename O, class T>
inline std::unique_ptr<O> make_unique(T* pointer) {
  return std::unique_ptr<O>(pointer);
}

class SyscallProxyImpl: public latte::SyscallProxy {
  public:
    int set_child_ports(pid_t pid, int lo, int hi) override {
      return ::set_child_ports(pid, lo, hi);
    }
    int get_local_ports(int* lo, int *hi) override {
      return ::get_local_ports(lo, hi);
    }
    int add_reserved_ports(int lo, int hi) override {
      return ::add_reserved_ports(lo, hi);
    }
    int del_reserved_ports(int lo, int hi) override {
      return ::del_reserved_ports(lo, hi);
    }
    int clear_reserved_ports() override {
      return ::clear_reserved_ports();
    }
};

}


namespace latte {

  CoreManager::CoreManager(const std::string& metadata_url, const std::string& myip,
      const std::string& persistence_path, std::unique_ptr<SyscallProxy> proxy):
    terminate_(false),
    myip_(std::move(myip)),
    persistence_path_(persistence_path),
    proxy_(std::move(proxy)) {
    int lo, hi;
    if (proxy_->get_local_ports(&lo, &hi) != 0) {
      throw std::runtime_error("error fetching ip local ports");
    }
    port_manager_ = make_unique<PortManager>((uint32_t) lo, (uint32_t) hi);
    /// TODO:
    // Refactor this: metadata service client needs not be a concrete type, hard
    // for testing and replacing
    client_ = make_unique<MetadataServiceClient>(metadata_url, format_id(myip_, lo, hi));
    config_root_[K_PRINCIPAL] = web::json::value::object();
    config_root_[K_IMAGE] = web::json::value::object();
    config_root_[K_ACCESSOR] = web::json::value::object();
  }

  CoreManager::~CoreManager() {
    {
      std::lock_guard<std::mutex> guard(this->write_lock_);
      terminate_ = true;
      this->sync_cond_.notify_one();
    }
    if (this->sync_thread_) {
      try {
        this->sync_thread_->join();
      } catch(std::system_error e) {
        log_err("error quiting the sync thread: %s", e.what());
      }
    }
  }

  CoreManager* CoreManager::from_disk(const std::string& fpath,
      const std::string& server_url, const std::string &myip) {
    auto manager = new CoreManager(server_url, std::move(myip), fpath,
        make_unique<SyscallProxyImpl>());
    std::string config_data = read_file(fpath);
    if (config_data == "") {
      log_err("not able to read config data, assume clean manager");
      return manager;
    } else {
      try {
        manager->set_new_config_root(web::json::value::parse(config_data));
        manager->sync();
      } catch(std::runtime_error e) {
        log_err("caught error in recovering the core library, %s", e.what());
      }
    }
    return manager;
  }

  bool CoreManager::has_principal(uint64_t id) const {
    return principals_.find(id) != principals_.cend();
  }
  bool CoreManager::has_principal_by_port(uint32_t port) const {
    return port_manager_->is_allocated(port);
  }
  bool CoreManager::has_image(std::string& hash) const {
    return images_.find(hash) != images_.end();
  }
  bool CoreManager::has_accessor(std::string& id) const {
    return accessors_.find(id) != accessors_.end();
  }

  void CoreManager::sync() noexcept {
    try {
      proxy_->clear_reserved_ports();
      sync_principals();
      sync_objects();
      sync_images();
    } catch(std::runtime_error e) {
      log_err("runtime error during syncing: %s", e.what());
    } catch (web::json::json_exception e) {
      log_err("json exception during syncing: %s", e.what());
    } catch (web::http::http_exception e) {
      log_err("http exception during syncing: %s", e.what());
    }

  }

  void CoreManager::sync_objects() {
    auto objs = config_root_[K_ACCESSOR].as_object();
    accessors_.clear();
    for (auto i = objs.begin(); i != objs.end(); ++i) {
      auto obj = AccessorObject::from_json(std::move(i->second));
      std::string name(obj.name());
      accessors_.emplace(name, make_unique<AccessorObject>(std::move(obj)));
    }
  }

  void CoreManager::sync_images() {
    auto images = config_root_[K_IMAGE].as_object();
    images_.clear();
    for (auto i = images.begin(); i != images.end(); ++i) {
      auto image = Image::from_json(std::move(i->second));
      std::string name(image.hash());
      images_.emplace(name, make_unique<Image>(std::move(image)));
    }
  }

  void CoreManager::sync_principals() {
    auto principals = config_root_[K_PRINCIPAL].as_object();
    principals_.clear();
    for (auto i = principals.begin(); i != principals.end(); ++i) {
      auto p = Principal::from_json(std::move(i->second));
      auto exec_path = check_proc(p.id());
      if (exec_path == "") {
        log("process %llu is not running! Syncing anyway\n", p.id());
      }
      principals_.emplace(p.id(), make_unique<Principal>(std::move(p)));
      port_manager_->allocate(p.lo(), p.hi());
      proxy_->set_child_ports(p.id(), p.lo(), p.hi());
      proxy_->add_reserved_ports(p.lo(), p.hi());
    }
  }

  int CoreManager::create_image(
      const std::string& image_hash,
      const std::string& source_url,
      const std::string& source_rev,
      const std::string& misc_conf
      ) noexcept {
    auto location = images_.emplace(image_hash,
        make_unique<Image>(image_hash, source_url, source_rev, misc_conf));

    try {
      client_->post_new_image(image_hash, source_url, source_rev, misc_conf);
      notify_created(K_IMAGE, std::string(image_hash), 
          location.first->second->to_json());
      return 0;
    } catch (std::runtime_error e) {
      images_.erase(image_hash);
      return -1;
    } catch (web::json::json_exception) {
      images_.erase(image_hash);
      return -2;
    } catch (web::http::http_exception) {
      images_.erase(image_hash);
      return -3;
    }
  }

  int CoreManager::create_principal(uint64_t id,
      const std::string& image_hash, const std::string& configs, int nport) noexcept {
    PortManager::PortPair prange;
    try {
      prange = port_manager_->allocate(nport);
    } catch (std::runtime_error) {
      return -1;
    }

    try {
      std::shared_ptr<Principal> newp = std::make_shared<Principal>(id,
            prange.first, prange.second, image_hash, configs);
      index_principals_[prange.first] = std::weak_ptr<Principal>(newp);
      std::weak_ptr<Principal> tmp(newp);
      principals_[id] = std::move(newp);

      client_->post_new_principal(format_principal_id(id), 
          myip_, prange.first, prange.second, image_hash, configs);
      proxy_->set_child_ports((pid_t)id, prange.first, prange.second);
      proxy_->add_reserved_ports(prange.first, prange.second);
      if (auto ref = tmp.lock()) {
        notify_created(K_PRINCIPAL, std::string(format_principal_id(id)),
            ref->to_json());
      }
    } catch (std::runtime_error) {
      port_manager_->deallocate(prange.first);
      index_principals_.erase(prange.first);
      principals_.erase(id);
      return -1;
    } catch (web::json::json_exception) {
      port_manager_->deallocate(prange.first);
      index_principals_.erase(prange.first);
      principals_.erase(id);
      return -2;
    } catch (web::http::http_exception) {
      port_manager_->deallocate(prange.first);
      index_principals_.erase(prange.first);
      principals_.erase(id);
      return -3;
    }
    return 0;
  }

  int CoreManager::post_object_acl(const std::string& obj_id,
      const std::string& requirement) noexcept {
    try {
      const auto insert_res = accessors_.emplace(obj_id,
          make_unique<AccessorObject>(obj_id));
      const auto& obj_ptr = insert_res.first->second;
      obj_ptr->add_acl(requirement);
      client_->post_object_acl(obj_id, requirement);
      notify_created(K_ACCESSOR, std::string(obj_id),
          insert_res.first->second->to_json());
    } catch(std::runtime_error) {
      accessors_.erase(obj_id);
      return -1;
    } catch (web::json::json_exception) {
      accessors_.erase(obj_id);
      return -2;
    } catch (web::http::http_exception) {
      accessors_.erase(obj_id);
      return -3;
    }
    return 0;
  }

  int CoreManager::endorse_image(const std::string& image_hash,
      const std::string& endorsement) noexcept {
    try {
      if (images_.find(image_hash) == images_.cend()) {
        log("Endorsing remote image: %s", image_hash.c_str());
      }
      client_->endorse_image(image_hash, endorsement);
    } catch(std::runtime_error) {
      return -1;
    } catch (web::json::json_exception) {
      return -2;
    } catch (web::http::http_exception) {
      return -3;
    }
    return 0;
  }

  bool CoreManager::attest_principal_property(const std::string& ip,
      uint32_t port, const std::string& property) noexcept {
    try {
      return client_->has_property(ip, port, property);
    } catch(std::runtime_error) {
      return false;
    } catch (web::json::json_exception) {
      return false;
    } catch (web::http::http_exception) {
      return false;
    }
  }

  bool CoreManager::attest_principal_access(const std::string& ip,
      uint32_t port, const std::string& obj) noexcept {
    try {
      return client_->has_property(ip, port, obj);
    } catch(std::runtime_error) {
      return false;
    } catch (web::json::json_exception) {
      return false;
    } catch (web::http::http_exception) {
      return false;
    }
  }

  int CoreManager::delete_principal(uint64_t uuid) noexcept {
    /// only delete principal and recycle the port at the moment
    auto record = principals_.find(uuid);
    if (record == principals_.cend()) {
      log("removing non-existed or non-process principal %llu\n", uuid);
      /// trying to diagnose the process pid, to see if it exist and there
      // is any unsync.
      auto exec_name = check_proc(uuid);
      if (exec_name != "") {
        log("diagnose: pid %llu exist, executing %s, but not in principal map",
            uuid, exec_name.c_str());
      }
      return -1;
    }
    notify_deleted(K_PRINCIPAL, format_principal_id(uuid));
    port_manager_->deallocate(record->second->lo());
    proxy_->del_reserved_ports(record->second->lo(), record->second->hi());
    index_principals_.erase(record->second->lo());
    principals_.erase(record);
    return 0;
  }

  void CoreManager::notify_created(std::string&& type, std::string&& key,
      web::json::value v) {
    std::unique_lock<std::mutex> guard(this->write_lock_);
    this->sync_q_.emplace_back(std::move(type), std::move(key), true, v);
    this->sync_cond_.notify_one();
  }

  void CoreManager::notify_deleted(std::string&& type, std::string&& key) {
    std::unique_lock<std::mutex> guard(this->write_lock_);
    this->sync_q_.emplace_back(std::move(type), std::move(key), false, 
        web::json::value::Null);
    this->sync_cond_.notify_one();
  }

  void CoreManager::start_sync_thread() {

    if (terminate_) {
      log_err("trying to start sync thread at terminate phase, potential bug");
      return;
    }
    log("starting latte sync thread for process %lu\n", getpid());
    sync_thread_ = make_unique<std::thread>([this]() noexcept {

      while (true) {
        decltype(this->sync_q_) pending;
        {
        /// We have to do this, because chrono::seconds require a reference, which
        // make it a ODR-use, which in addition requires definition of SYNC_DURATION.
        // We don't want the definition for real!
          constexpr int duration = SYNC_DURATION;
          std::unique_lock<std::mutex> guard(this->write_lock_);
          this->sync_cond_.wait_for(guard, std::chrono::seconds(duration),
              [this] { return this->sync_q_.size() > 0 || this->terminate_; });
          // Quit the loop for terminate signal
          if (this->terminate_) {
            break;
          }
          pending.swap(this->sync_q_);
        }
        /// tuple[0] is the class type, principal, accessor, or images
        for (auto i = pending.begin(); i != pending.end(); i++) {
          auto& collection = this->config_root_[std::get<0>(*i)];
          if (std::get<2>(*i)) { // true means insertion
            printf("adding %s with %s\n", std::get<1>(*i).c_str(), std::get<3>(*i).serialize().c_str());
            collection[std::get<1>(*i)] = std::get<3>(*i);
          } else {
            printf("deleting %s\n", std::get<1>(*i).c_str());
            collection.erase(std::get<1>(*i));
          }
        }
        log("syncing config at %s", format_system_time().c_str());
        this->save(this->persistence_path_);
        pending.clear(); /// just for defensive if in case dctor makes some magic..
      }
    });
  }

  void CoreManager::save(const std::string &fpath) noexcept {
    try {
      std::ofstream out(fpath);
      out << this->config_root_.serialize();
    } catch(std::ios_base::failure e) {
      log_err("IO failure during save config: %s", e.what());
    } catch(std::runtime_error e) {
      log_err("something wrong happens during save config: %s", e.what());
    } catch(web::json::json_exception e) {
      log_err("json error during save config: %s", e.what());
    }
  }

}


static std::unique_ptr<latte::CoreManager> core;

// A list of principals, images and objects maintaned by this library

int libport_init(const char *server_url, const char *persistence_path) {
  try {
    std::string myip = get_myip();
    if (myip.compare("") == 0) {
      latte::log_err("failed to initialize libport principal identity");
      return -1;
    }
    if (!persistence_path || strcmp(persistence_path, "") == 0) {
      core = make_unique<latte::CoreManager>(server_url, std::move(myip),
          make_default_restore_path(getpid()), make_unique<SyscallProxyImpl>());
    } else {
      core = make_unique<latte::CoreManager>(server_url, std::move(myip),
          persistence_path, make_unique<SyscallProxyImpl>());
    }
    core->start_sync_thread();
  } catch (std::runtime_error) {
    return -1;
  }
  return 0;
}

int libport_reinit(const char *server_url, const char *persistence_path) {
  try {
    std::string myip = get_myip();
    if (myip.compare("") == 0) {
      latte::log_err("failed to initialize libport principal identity");
      return -1;
    }
    core = std::unique_ptr<latte::CoreManager>(
        latte::CoreManager::from_disk(persistence_path, server_url, myip));
    core->start_sync_thread();
  } catch (std::runtime_error) {
    return -1;
  }
  return 0;

}

#define CHECK_LIB_INIT \
  if (!core) { \
    latte::log_err("the library is not initialized");\
    return -1; \
  }

#define CHECK_LIB_INIT_BOOL \
  if (!core) { \
    latte::log_err("the library is not initialized");\
    return false; \
  }

int create_principal(uint64_t uuid, const char *image, const char *config,
    int nport) {
  // create principal, addresses are assgined by the library. Any statement
  // appended will be included must be "const char*" type, which means String
  // in Java. It will be included in misc config fields
  CHECK_LIB_INIT;
  latte::log("creating principal %llu, %s, %s, %d\n", uuid, image, config, nport);
  return core->create_principal(uuid, image, config, nport);
}


int create_image(const char *image_hash, const char *source_url,
    const char *source_rev, const char *misc_conf) {
  CHECK_LIB_INIT;
  latte::log("creating image %s, %s, %s, %s\n", image_hash, source_url,
      source_rev, misc_conf);
  return core->create_image(image_hash, source_url, source_rev, misc_conf);
}

int post_object_acl(const char *obj_id, const char *requirement) {
  CHECK_LIB_INIT;
  latte::log("posting obj acl: %s, %s\n", obj_id, requirement);
  return core->post_object_acl(obj_id, requirement);
}

int endorse_image(const char *image_hash, const char *endorsement) {
  CHECK_LIB_INIT;
  latte::log("endorsing image: %s, %s\n", image_hash, endorsement);
  return core->post_object_acl(image_hash, endorsement);
}

bool attest_principal_property(const char *ip, uint32_t port, const char *prop) {
  CHECK_LIB_INIT_BOOL;
  latte::log("attesting property: %s, %u, %s\n", ip, port, prop);
  return core->attest_principal_property(ip, port, prop);
}

bool attest_principal_access(const char *ip, uint32_t port, const char *obj) {
  CHECK_LIB_INIT_BOOL;
  latte::log("attesting access: %s, %u, %s\n", ip, port, obj);
  return core->attest_principal_access(ip, port, obj);
}

int delete_principal(uint64_t uuid) {
  CHECK_LIB_INIT_BOOL;
  latte::log("deleting principal: %llu\n", uuid);
  return core->delete_principal(uuid);
}





