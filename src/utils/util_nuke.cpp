#include <mist/comms.h>
#include <mist/config.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/timing.h>
#include <mist/util.h>

#include <dirent.h>
#include <fstream>
#include <unistd.h>

const char * getStateString(uint8_t state){
  switch (state){
  case STRMSTAT_OFF: return "Stream is offline";
  case STRMSTAT_INIT: return "Stream is initializing";
  case STRMSTAT_BOOT: return "Stream is booting";
  case STRMSTAT_WAIT: return "Stream is waiting for data";
  case STRMSTAT_READY: return "Stream is online";
  case STRMSTAT_SHUTDOWN: return "Stream is shutting down";
  case STRMSTAT_INVALID: return "Stream status is invalid?!";
  default: return "Stream status is unknown?!";
  }
}

/// Gets a PID from a shared memory page, if it exists
uint64_t getPidFromPage(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::sharedPage pidPage(pageName, 8, false, false);
  if (pidPage){
    return *(uint64_t*)(pidPage.mapped);
  }
  return 0;
}

/// Deletes a shared memory page, if it exists
void nukePage(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::sharedPage page(pageName, 0, false, false);
  page.master = true;
}

/// Deletes a semaphore, if it exists
void nukeSem(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::semaphore sem(pageName, O_RDWR, ACCESSPERMS, 0, true);
  if (sem){sem.unlink();}
}

/// Scans /proc for MistIn* processes started with "-s <stream>" (the form Util::startInput uses)
/// and adds their pids to checkPids. This is the fallback for when the pid pages in shared memory
/// are missing or stale, in which case the regular detection below finds nothing to kill even
/// though an input (typically a wedged pull process and its angel) is still running.
void findInputsByCmdline(const std::string & stream, std::set<pid_t> & checkPids) {
  DIR * d = opendir("/proc");
  if (!d) { return; }
  pid_t self = getpid();
  struct dirent * ent;
  while ((ent = readdir(d))) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9') { continue; }
    pid_t pid = atoi(ent->d_name);
    if (pid <= 1 || pid == self) { continue; }
    std::ifstream f((std::string("/proc/") + ent->d_name + "/cmdline").c_str(), std::ios::binary);
    if (!f) { continue; }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.empty()) { continue; }
    // Split on NUL into argv
    std::deque<std::string> argv;
    size_t start = 0;
    while (start < raw.size()) {
      size_t end = raw.find('\0', start);
      if (end == std::string::npos) { end = raw.size(); }
      argv.push_back(raw.substr(start, end - start));
      start = end + 1;
    }
    if (argv.empty()) { continue; }
    std::string binName = argv[0];
    size_t slash = binName.rfind('/');
    if (slash != std::string::npos) { binName.erase(0, slash + 1); }
    if (binName.substr(0, 6) != "MistIn") { continue; }
    for (size_t i = 1; i + 1 < argv.size(); ++i) {
      if ((argv[i] == "-s" || argv[i] == "--stream") && argv[i + 1] == stream) {
        INFO_MSG("Found %s process %" PRIu64 " for stream %s via /proc scan", binName.c_str(), (uint64_t)pid, stream.c_str());
        checkPids.insert(pid);
        break;
      }
    }
  }
  closedir(d);
}

// Remove process that are no longer running from the process list
void cleanUpProcessList(std::set<pid_t> & checkPids) {
  if (checkPids.size()) {
    std::set<pid_t> gonePids;
    for (auto & P : checkPids) {
      if (!Util::Procs::isRunning(P)) { gonePids.insert(P); }
    }
    for (auto & P : gonePids) { checkPids.erase(P); }
  }
}

void killProcesses(std::set<pid_t> & checkPids, const char *procType) {
  cleanUpProcessList(checkPids);
  if (checkPids.size()) {
    // Wait a bit to settle
    Util::sleep(1000);
    cleanUpProcessList(checkPids);
  }
  // Hard-kill any remaining processes
  if (checkPids.size()) {
    WARN_MSG("Hard killing %zu %s processes", checkPids.size(), procType);
    while (checkPids.size()) {
      INFO_MSG("Hard killing %s process %" PRIu64, procType, (uint64_t)*checkPids.begin());
      Util::Procs::Murder(*checkPids.begin());
      checkPids.erase(*checkPids.begin());
    }
  }
}

// Main semaphore for SEM_LIVE lock
IPC::semaphore mainSem, pullSem;
char mainSemName[NAME_BUFFER_SIZE], pullSemName[NAME_BUFFER_SIZE];

/// Attempts to lock the SEM_LIVE semaphore for the stream, only if not already locked.
/// Returns current lock status
bool tryLock() {
  if (!mainSem.locked()) {
    mainSem.open(mainSemName, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    if (mainSem.tryWait()) { INFO_MSG("Placed input lock"); }
  }
  if (!pullSem.locked()) {
    pullSem.open(pullSemName, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    if (pullSem.tryWait()) { INFO_MSG("Placed pull lock"); }
  }
  return mainSem.locked() && pullSem.locked();
}

int main(int argc, char **argv){
  Util::redirectLogsIfNeeded();
  if (argc < 1){
    FAIL_MSG("Usage: %s STREAM_NAME", argv[0]);
    return 1;
  }
  Util::setStreamName(argv[1]);

  // Track process IDs that we want to ensure are fully off by the time we finish
  std::set<pid_t> checkPids;

  // Write stream name into mainSemName / pullSemName
  snprintf(mainSemName, NAME_BUFFER_SIZE, SEM_INPUT, Util::streamName);
  snprintf(pullSemName, NAME_BUFFER_SIZE, "/MstSemPull_%s", Util::streamName);
  tryLock();

  uint8_t state = Util::getStreamStatus(Util::streamName);
  INFO_MSG("Current stream status: %s", getStateString(state));
  uint64_t startTime = Util::bootMS();
  if (state != STRMSTAT_OFF){INFO_MSG("Attempting clean shutdown...");}
  while (state != STRMSTAT_OFF && Util::bootMS() < startTime + 5000) {
    uint64_t pid;
    pid = getPidFromPage(SHM_STREAM_IPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
    }
    pid = getPidFromPage(SHM_STREAM_PPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
    }
    if (!tryLock()) {
      Util::wait(1);
      if (!tryLock()) {
        Util::wait(2);
        tryLock();
      }
    }
    uint8_t prevState = state;
    state = Util::getStreamStatus(Util::streamName);
    if (prevState != state){
      INFO_MSG("Current stream status: %s", getStateString(state));
    }
    Util::wait(10);
    tryLock();
  }

  // Ensure we have the input lock, one way or another
  if (!tryLock()) {
    if (!mainSem.locked()) {
      INFO_MSG("Breaking input lock forcefully...");
      mainSem.unlink();
    }
    if (!pullSem.locked()) {
      INFO_MSG("Breaking pull lock forcefully...");
      pullSem.unlink();
    }
    if (!tryLock()) {
      FAIL_MSG("Could not force input and/or pull lock..? Aborting!");
      return 2;
    }
  }

  INFO_MSG("Detecting running inputs...");
  // Scoping to clear up metadata and track providers
  {
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_META, argv[1]);
    IPC::sharedPage streamPage(pageName, 0, false, false);
    if (streamPage.mapped) {
      streamPage.master = true;
      Util::RelAccX stream(streamPage.mapped, false);
      if (stream.isReady()) {
        Util::RelAccX trackList(stream.getPointer("tracks"), false);
        if (trackList.isReady()) {
          for (size_t i = 0; i < trackList.getPresent(); i++) {
            IPC::sharedPage trackPage(trackList.getPointer("page", i), SHM_STREAM_TRACK_LEN, false, false);
            trackPage.master = true;
            pid_t pid = trackList.getInt("pid", i);
            if (pid > 1) {
              Util::Procs::Stop(pid);
              checkPids.insert(pid);
            }
          }
        }
      }
    }
  }
  { // Double-check input and pull input process numbers just in case
    uint64_t pid;
    pid = getPidFromPage(SHM_STREAM_IPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
    }
    pid = getPidFromPage(SHM_STREAM_PPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
    }
  }
  { // Fallback: the pid pages may be gone while the processes are not. Scan /proc for inputs of this stream.
    std::set<pid_t> scanned;
    findInputsByCmdline(argv[1], scanned);
    for (auto & P : scanned) {
      if (!checkPids.count(P)) {
        Util::Procs::Stop(P);
        checkPids.insert(P);
      }
    }
  }
  killProcesses(checkPids, "input");
  INFO_MSG("Detecting and wiping leftovers in shared memory...");
  // Scoping to clear up metadata and track providers
  {
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_META, argv[1]);
    IPC::sharedPage streamPage(pageName, 0, false, false);
    if (streamPage.mapped){
      streamPage.master = true;
      Util::RelAccX stream(streamPage.mapped, false);
      if (stream.isReady()){
        Util::RelAccX trackList(stream.getPointer("tracks"), false);
        if (trackList.isReady()){
          for (size_t i = 0; i < trackList.getPresent(); i++){
            IPC::sharedPage trackPage(trackList.getPointer("page", i), SHM_STREAM_TRACK_LEN, false, false);
            trackPage.master = true;
            pid_t pid = trackList.getInt("pid", i);
            if (pid > 1){
              Util::Procs::Stop(pid);
              checkPids.insert(pid);
            }
            if (trackPage){
              Util::RelAccX track(trackPage.mapped, false);
              if (track.isReady()){
                Util::RelAccX pages(track.getPointer("pages"), false);
                if (pages.isReady()){
                  for (uint64_t j = pages.getDeleted(); j < pages.getEndPos(); j++){
                    char thisPageName[NAME_BUFFER_SIZE];
                    snprintf(thisPageName, NAME_BUFFER_SIZE, SHM_TRACK_DATA,
                             argv[1], i, (uint32_t)pages.getInt("firstkey", j));
                    IPC::sharedPage p(thisPageName, 0);
                    p.master = true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  //Wipe relevant pages
  nukePage(SHM_STREAM_STATE);
  nukePage(SHM_STREAM_IPID);
  nukePage(SHM_STREAM_PPID);
  // Scoping to clear up users page
  {
    Comms::Users cleanUsers;
    cleanUsers.reload(Util::streamName, true);
    std::set<pid_t> checkPids;
    for (size_t i = 0; i < cleanUsers.recordCount(); ++i){
      uint8_t status = cleanUsers.getStatus(i);
      cleanUsers.setStatus(COMM_STATUS_INVALID, i);
      if (status != COMM_STATUS_INVALID && !(status & COMM_STATUS_DISCONNECT)){
        pid_t pid = cleanUsers.getPid(i);
        if (pid > 1 && !(cleanUsers.getStatus(i) & COMM_STATUS_NOKILL)){
          Util::Procs::Stop(pid);
          checkPids.insert(pid);
        }
      }
    }
    cleanUsers.setMaster(true);
  }
  killProcesses(checkPids, "output");
  nukePage(COMMS_USERS);
  nukeSem(SEM_USERS);
  nukeSem(SEM_LIVE);
  nukeSem(SEM_TRACKLIST);
  // Finally, remove the input and pull lock semaphores
  pullSem.unlink();
  mainSem.unlink();
  INFO_MSG("Completed cleanup");
  return 0;
}

