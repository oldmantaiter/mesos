/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <list>
#include <map>
#include <string>

#include <process/defer.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>

#include "docker/docker.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/docker.hpp"

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"

#include "usage/usage.hpp"


using std::list;
using std::map;
using std::string;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;


// Declared in header, see explanation there.
// TODO(benh): At some point to run multiple slaves we'll need to make
// the Docker container name creation include the slave ID.
string DOCKER_NAME_PREFIX = "mesos-";


class DockerContainerizerProcess
  : public process::Process<DockerContainerizerProcess>
{
public:
  DockerContainerizerProcess(
      const Flags& _flags,
      const Docker& _docker)
    : flags(_flags),
      docker(_docker) {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(
      const ContainerID& containerId,
      bool killed = true); // process is either killed or reaped.

  virtual process::Future<hashset<ContainerID> > containers();

private:
  // Continuations and helpers.
  process::Future<Nothing> _recover(
      const std::list<Docker::Container>& containers);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> __launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint,
      const Docker::Container& container);

  void _destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Nothing>& future);

  void __destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Option<int> >& status);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources,
      const Docker::Container& container);

  Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Docker::Container& container);

  // Call back for when the executor exits. This will trigger
  // container destroy.
  void reaped(const ContainerID& containerId);

  static std::string containerName(const ContainerID& containerId);

  const Flags flags;

  Docker docker;

  // TODO(idownes): Consider putting these per-container variables into a
  // struct.
  // Promises for futures returned from wait().
  hashmap<ContainerID,
    process::Owned<process::Promise<containerizer::Termination> > > promises;

  // We need to keep track of the future exit status for each executor because
  // we'll only get a single notification when the executor exits.
  hashmap<ContainerID, process::Future<Option<int> > > statuses;

  // We keep track of the resources for each container so we can set the
  // ResourceStatistics limits in usage().
  hashmap<ContainerID, Resources> resources;

  // Set of containers that are in process of being destroyed.
  hashset<ContainerID> destroying;
};


// Parse the ContainerID from a Docker container and return None if
// the container was not launched from Mesos.
Option<ContainerID> parse(
    const Docker::Container& container)
{
  Option<string> name = None();

  if (strings::startsWith(container.name, DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, DOCKER_NAME_PREFIX, strings::PREFIX);
  } else if (strings::startsWith(container.name, "/" + DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, "/" + DOCKER_NAME_PREFIX, strings::PREFIX);
  }

  if (name.isSome()) {
    ContainerID id;
    id.set_value(name.get());
    return id;
  }

  return None();
}


Try<DockerContainerizer*> DockerContainerizer::create(
    const Flags& flags)
{
  Try<Docker> docker = Docker::create(flags.docker);
  if (docker.isError()) {
    return Error(docker.error());
  }

  return new DockerContainerizer(flags, docker.get());
}


DockerContainerizer::DockerContainerizer(
    const Flags& flags,
    const Docker& docker)
{
  process = new DockerContainerizerProcess(flags, docker);
  spawn(process);
}


DockerContainerizer::~DockerContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> DockerContainerizer::recover(
    const Option<SlaveState>& state)
{
  return dispatch(process, &DockerContainerizerProcess::recover, state);
}


Future<bool> DockerContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &DockerContainerizerProcess::launch,
                  containerId,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<bool> DockerContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &DockerContainerizerProcess::launch,
                  containerId,
                  taskInfo,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<Nothing> DockerContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(process,
                  &DockerContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> DockerContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &DockerContainerizerProcess::usage, containerId);
}


Future<containerizer::Termination> DockerContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &DockerContainerizerProcess::wait, containerId);
}


void DockerContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &DockerContainerizerProcess::destroy, containerId, true);
}


Future<hashset<ContainerID> > DockerContainerizer::containers()
{
  return dispatch(process, &DockerContainerizerProcess::containers);
}


// A Subprocess async-safe "setup" helper used by
// DockerContainerizerProcess when launching the mesos-executor that
// does a 'setsid' and then synchronizes with the parent.
static int setup(const string& directory)
{
  // Put child into its own process session to prevent slave suicide
  // on child process SIGKILL/SIGTERM.
  if (::setsid() == -1) {
    return errno;
  }

  // Run the process in the specified directory.
  if (!directory.empty()) {
    if (::chdir(directory.c_str()) == -1) {
      return errno;
    }
  }

  // Synchronize with parent process by reading a byte from stdin.
  char c;
  ssize_t length;
  while ((length = read(STDIN_FILENO, &c, sizeof(c))) == -1 && errno == EINTR);

  if (length != sizeof(c)) {
    // This will occur if the slave terminates during executor launch.
    // There's a reasonable probability this will occur during slave
    // restarts across a large/busy cluster.
    ABORT("Failed to synchronize with slave (it has probably exited)");
  }

  return 0;
}


string DockerContainerizerProcess::containerName(const ContainerID& containerId)
{
  return DOCKER_NAME_PREFIX + stringify(containerId);
}


Future<Nothing> DockerContainerizerProcess::recover(
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering Docker containers";

  if (state.isSome()) {
    // Collection of pids that we've started reaping in order to
    // detect very unlikely duplicate scenario (see below).
    hashmap<ContainerID, pid_t> pids;

    foreachvalue (const FrameworkState& framework, state.get().frameworks) {
      foreachvalue (const ExecutorState& executor, framework.executors) {
        if (executor.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its info could not be recovered";
          continue;
        }

        if (executor.latest.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its latest run could not be recovered";
          continue;
        }

        // We are only interested in the latest run of the executor!
        const ContainerID& containerId = executor.latest.get();
        Option<RunState> run = executor.runs.get(containerId);
        CHECK_SOME(run);
        CHECK_SOME(run.get().id);
        CHECK_EQ(containerId, run.get().id.get());

        // We need the pid so the reaper can monitor the executor so
        // skip this executor if it's not present. This is not an
        // error because the slave will try to wait on the container
        // which will return a failed Termination and everything will
        // get cleaned up.
        if (!run.get().forkedPid.isSome()) {
          continue;
        }

        if (run.get().completed) {
          VLOG(1) << "Skipping recovery of executor '" << executor.id
                  << "' of framework " << framework.id
                  << " because its latest run "
                  << containerId << " is completed";
          continue;
        }

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
                  << "' of framework " << framework.id;

        // Save a termination promise.
        Owned<Promise<containerizer::Termination> > promise(
            new Promise<containerizer::Termination>());

        promises.put(containerId, promise);

        pid_t pid = run.get().forkedPid.get();

        statuses[containerId] = process::reap(pid);

        statuses[containerId]
          .onAny(defer(self(), &Self::reaped, containerId));

        if (pids.containsValue(pid)) {
          // This should (almost) never occur. There is the
          // possibility that a new executor is launched with the same
          // pid as one that just exited (highly unlikely) and the
          // slave dies after the new executor is launched but before
          // it hears about the termination of the earlier executor
          // (also unlikely).
          return Failure(
              "Detected duplicate pid " + stringify(pid) +
              " for container " + stringify(containerId));
        }

        pids.put(containerId, pid);
      }
    }
  }

  // Get the list of all Docker containers (running and exited) in
  // order to remove any orphans.
  return docker.ps(true, DOCKER_NAME_PREFIX)
    .then(defer(self(), &Self::_recover, lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_recover(
    const list<Docker::Container>& containers)
{
  foreach (const Docker::Container& container, containers) {
    VLOG(1) << "Checking if Docker container named '"
            << container.name << "' was started by Mesos";

    Option<ContainerID> id = parse(container);

    // Ignore containers that Mesos didn't start.
    if (id.isNone()) {
      continue;
    }

    VLOG(1) << "Checking if Mesos container with ID '"
            << stringify(id.get()) << "' has been orphaned";

    // Check if we're watching an executor for this container ID and
    // if not, rm -f the Docker container.
    if (!statuses.keys().contains(id.get())) {
      // TODO(benh): Retry 'docker rm -f' if it failed but the container
      // still exists (asynchronously).
      docker.kill(container.id, true);
    }
  }

  return Nothing();
}


Future<bool> DockerContainerizerProcess::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (promises.contains(containerId)) {
    return Failure("Container already started");
  }

  CommandInfo command = executorInfo.command();

  if (!command.has_container()) {
    LOG(INFO) << "No container info found, skipping launch";
    return false;
  }

  string image = command.container().image();

  // Check if we should try and launch this command.
  if (!strings::startsWith(image, "docker:///")) {
    LOG(INFO) << "No docker image found, skipping launch";
    return false;
  }

  Owned<Promise<containerizer::Termination> > promise(
    new Promise<containerizer::Termination>());

  promises.put(containerId, promise);

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' and framework '" << executorInfo.framework_id() << "'";

  // Extract the Docker image.
  image = strings::remove(image, "docker:///", strings::PREFIX);

  // Construct the Docker container name.
  string name = containerName(containerId);

  map<string, string> env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Include any environment variables from CommandInfo.
  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    env[variable.name()] = variable.value();
  }

  Resources resources = executorInfo.resources();

  // Start a docker container then launch the executor (but destroy
  // the Docker container if launching the executor failed).
  return docker.run(image, command.value(), name, resources, env)
    .then(defer(self(),
               &Self::_launch,
               containerId,
               executorInfo,
               slaveId,
               slavePid,
               checkpoint))
    .onFailed(defer(self(), &Self::destroy, containerId, false));
}


Future<bool> DockerContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (promises.contains(containerId)) {
    return Failure("Container already started");
  }

  if (!taskInfo.has_command()) {
    LOG(WARNING) << "Not expecting call without command info";
    return false;
  }

  const CommandInfo& command = taskInfo.command();

  // Check if we should try and launch this command.
  if (!command.has_container() ||
      !strings::startsWith(command.container().image(), "docker:///")) {
    LOG(INFO) << "No container info or container image is not docker image, "
              << "skipping launch";
    return false;
  }

  Owned<Promise<containerizer::Termination> > promise(
      new Promise<containerizer::Termination>());

  promises.put(containerId, promise);

  // Store the resources for usage().
  resources.put(containerId, taskInfo.resources());

  LOG(INFO) << "Starting container '" << containerId
            << "' for task '" << taskInfo.task_id()
            << "' (and executor '" << executorInfo.executor_id()
            << "') of framework '" << executorInfo.framework_id() << "'";

  // Extract the Docker image.
  string image = command.container().image();
  image = strings::remove(image, "docker:///", strings::PREFIX);

  // Construct the Docker container name.
  string name = containerName(containerId);

  // Start a docker container then launch the executor (but destroy
  // the Docker container if launching the executor failed).
  return docker.run(image, command.value(), name, taskInfo.resources())
    .then(defer(self(),
                &Self::_launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint))
    .onFailed(defer(self(), &Self::destroy, containerId, false));
}


Future<bool> DockerContainerizerProcess::_launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  // Prepare environment variables for the executor.
  map<string, string> env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Include any enviroment variables from CommandInfo.
  foreach (const Environment::Variable& variable,
           executorInfo.command().environment().variables()) {
    env[variable.name()] = variable.value();
  }

  // Construct the mesos-executor "override" to do a 'docker wait'
  // using the "name" we gave the container (to distinguish it from
  // Docker containers not created by Mesos). Note, however, that we
  // don't want the exit status from 'docker wait' but rather the exit
  // status from the container, hence the use of /bin/bash.
  string override =
    "/bin/sh -c 'exit `" +
    flags.docker + " wait " + containerName(containerId) + "`'";

  Try<Subprocess> s = subprocess(
      executorInfo.command().value() + " --override " + override,
      Subprocess::PIPE(),
      Subprocess::PATH(path::join(directory, "stdout")),
      Subprocess::PATH(path::join(directory, "stderr")),
      env,
      lambda::bind(&setup, directory));

  if (s.isError()) {
    return Failure("Failed to fork executor: " + s.error());
  }

  // Checkpoint the executor's pid if requested.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        executorInfo.framework_id(),
        executorInfo.executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing executor's forked pid "
              << s.get().pid() << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(s.get().pid()));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint executor's forked pid to '"
                 << path << "': " << checkpointed.error();

      // Close the subprocess's stdin so that it aborts.
      CHECK_SOME(s.get().in());
      os::close(s.get().in().get());

      return Failure("Could not checkpoint executor's pid");
    }
  }

  // Checkpoing complete, now synchronize with the process so that it
  // can continue to execute.
  CHECK_SOME(s.get().in());
  char c;
  ssize_t length;
  while ((length = write(s.get().in().get(), &c, sizeof(c))) == -1 &&
         errno == EINTR);

  if (length != sizeof(c)) {
    string error = string(strerror(errno));
    os::close(s.get().in().get());
    return Failure("Failed to synchronize with child process: " + error);
  }

  // And finally watch for when the executor gets reaped.
  statuses[containerId] = process::reap(s.get().pid());

  statuses[containerId]
    .onAny(defer(self(), &Self::reaped, containerId));

  return true;
}


Future<bool> DockerContainerizerProcess::_launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return docker.inspect(containerName(containerId))
    .then(defer(self(),
                &Self::__launch,
                containerId,
                executorInfo,
                slaveId,
                slavePid,
                checkpoint,
                lambda::_1));
}


Future<bool> DockerContainerizerProcess::__launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    const Docker::Container& container)
{
  Option<int> pid = container.pid;

  if (!pid.isSome()) {
    return Failure("Unable to get executor pid after launch");
  }

  if (checkpoint) {
    // TODO(tnachen): We might not be able to checkpoint if the slave
    // dies before it can checkpoint while the executor is still
    // running. Optinally we can consider recording the slave id and
    // executor id as part of the docker container name so we can
    // recover from this.
    const string& path =
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          slaveId,
          executorInfo.framework_id(),
          executorInfo.executor_id(),
          containerId);

    LOG(INFO) << "Checkpointing executor's forked pid "
              << pid.get() << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(pid.get()));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint executor's forked pid to '"
                 << path << "': " << checkpointed.error();

      return Failure("Could not checkpoint executor's pid");
    }
  }

  statuses[containerId] = process::reap(pid.get());

  statuses[containerId]
    .onAny(defer(self(), &Self::reaped, containerId));

  return true;
}


Future<Nothing> DockerContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& _resources)
{
  if (!promises.contains(containerId)) {
    LOG(WARNING) << "Ignoring updating unknown container: "
                 << containerId;
    return Nothing();
  }

  // Store the resources for usage()
  resources.put(containerId, _resources);

#ifdef __linux__
  if (!_resources.cpus().isSome() && !_resources.mem().isSome()) {
    LOG(WARNING) << "Ignoring update as no supported resources are present";
    return Nothing();
  }

  return docker.inspect(containerName(containerId))
    .then(defer(self(), &Self::_update, containerId, _resources, lambda::_1));
#else
  return Nothing();
#endif // __linux__
}


Future<Nothing> DockerContainerizerProcess::_update(
    const ContainerID& containerId,
    const Resources& _resources,
    const Docker::Container& container)
{
#ifdef __linux__
  // Determine the the cgroups hierarchies where the 'cpu' and
  // 'memory' subsystems are mounted (they may be the same). Note that
  // we make these static so we can reuse the result for subsequent
  // calls.
  static Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  static Result<string> memoryHierarchy = cgroups::hierarchy("memory");

  if (cpuHierarchy.isError()) {
    return Failure("Failed to determine the cgroup hierarchy "
                   "where the 'cpu' subsystem is mounted: " +
                   cpuHierarchy.error());
  }

  if (memoryHierarchy.isError()) {
    return Failure("Failed to determine the cgroup hierarchy "
                   "where the 'memory' subsystem is mounted: " +
                   memoryHierarchy.error());
  }

  // We need to find the cgroup(s) this container is currently running
  // in for both the hierarchy with the 'cpu' subsystem attached and
  // the hierarchy with the 'memory' subsystem attached so we can
  // update the proper cgroup control files.

  // First check that this container still appears to be running.
  Option<pid_t> pid = container.pid;
  if (pid.isNone()) {
    return Nothing();
  }

  // Determine the cgroup for the 'cpu' subsystem (based on the
  // container's pid).
  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());

  if (cpuCgroup.isError()) {
    return Failure("Failed to determine cgroup for the 'cpu' subsystem: " +
                   cpuCgroup.error());
  } else if (cpuCgroup.isNone()) {
    LOG(WARNING) << "Container " << containerId
                 << " does not appear to be a member of a cgroup "
                 << "where the 'cpu' subsystem is mounted";
  }

  // And update the CPU shares (if applicable).
  if (cpuHierarchy.isSome() &&
      cpuCgroup.isSome() &&
      _resources.cpus().isSome()) {
    double cpuShares = _resources.cpus().get();

    uint64_t shares =
      std::max((uint64_t) (CPU_SHARES_PER_CPU * cpuShares), MIN_CPU_SHARES);

    Try<Nothing> write =
      cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get(), shares);

    if (write.isError()) {
      return Failure("Failed to update 'cpu.shares': " + write.error());
    }

    LOG(INFO) << "Updated 'cpu.shares' to " << shares
              << " at " << path::join(cpuHierarchy.get(), cpuCgroup.get())
              << " for container " << containerId;
  }

  // Now determine the cgroup for the 'memory' subsystem.
  Result<string> memoryCgroup = cgroups::memory::cgroup(pid.get());

  if (memoryCgroup.isError()) {
    return Failure("Failed to determine cgroup for the 'memory' subsystem: " +
                   memoryCgroup.error());
  } else if (memoryCgroup.isNone()) {
    LOG(WARNING) << "Container " << containerId
                 << " does not appear to be a member of a cgroup "
                 << "where the 'memory' subsystem is mounted";
  }

  // And update the memory limits (if applicable).
  if (memoryHierarchy.isSome() &&
      memoryCgroup.isSome() &&
      _resources.mem().isSome()) {
    // TODO(tnachen): investigate and handle OOM with docker.
    Bytes mem = _resources.mem().get();
    Bytes limit = std::max(mem, MIN_MEMORY);

    // Always set the soft limit.
    Try<Nothing> write =
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get(), limit);

    if (write.isError()) {
      return Failure("Failed to set 'memory.soft_limit_in_bytes': " +
                     write.error());
    }

    LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to " << limit
              << " for container " << containerId;

    // Read the existing limit.
    Try<Bytes> currentLimit =
      cgroups::memory::limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get());

    if (currentLimit.isError()) {
      return Failure("Failed to read 'memory.limit_in_bytes': " +
                     currentLimit.error());
    }

    // Only update if new limit is higher.
    // TODO(benh): Introduce a MemoryWatcherProcess which monitors the
    // discrepancy between usage and soft limit and introduces a
    // "manual oom" if necessary.
    if (limit > currentLimit.get()) {
      write = cgroups::memory::limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get(), limit);

      if (write.isError()) {
        return Failure("Failed to set 'memory.limit_in_bytes': " +
                       write.error());
      }

      LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << limit << " at "
                << path::join(memoryHierarchy.get(), memoryCgroup.get())
                << " for container " << containerId;
    }
  }
#endif // __linux__

  return Nothing();
}


Future<ResourceStatistics> DockerContainerizerProcess::usage(
    const ContainerID& containerId)
{
#ifndef __linux__
  return Failure("Does not support usage() on non-linux platform");
#else
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  if (destroying.contains(containerId)) {
    return Failure("Container is being removed: " + stringify(containerId));
  }

  // Construct the Docker container name.
  string name = containerName(containerId);
  return docker.inspect(name)
    .then(defer(self(), &Self::_usage, containerId, lambda::_1));
#endif // __linux__
}


Future<ResourceStatistics> DockerContainerizerProcess::_usage(
    const ContainerID& containerId,
    const Docker::Container& container)
{
  Option<pid_t> pid = container.pid;
  if (pid.isNone()) {
    return Failure("Container is not running");
  }

  // Note that here getting the root pid is enough because
  // the root process acts as an 'init' process in the docker
  // container, so no other child processes will escape it.
  Try<ResourceStatistics> statistics =
    mesos::internal::usage(pid.get(), true, true);
  if (statistics.isError()) {
    return Failure(statistics.error());
  }

  ResourceStatistics result = statistics.get();

  // Set the resource allocations.
  Resources resource = resources[containerId];
  Option<Bytes> mem = resource.mem();
  if (mem.isSome()) {
    result.set_mem_limit_bytes(mem.get().bytes());
  }

  Option<double> cpus = resource.cpus();
  if (cpus.isSome()) {
    result.set_cpus_limit(cpus.get());
  }

  return result;
}


Future<containerizer::Termination> DockerContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return promises[containerId]->future();
}


void DockerContainerizerProcess::destroy(
    const ContainerID& containerId,
    bool killed)
{
  if (!promises.contains(containerId)) {
    LOG(WARNING) << "Ignoring destroy of unknown container: " << containerId;
    return;
  }

  if (destroying.contains(containerId)) {
    // Destroy has already been initiated.
    return;
  }

  destroying.insert(containerId);

  LOG(INFO) << "Destroying container '" << containerId << "'";

  // Do a 'docker rm -f' which we'll then find out about in '_wait'
  // after we've reaped either the container's root process (in the
  // event that we had just launched a container for an executor) or
  // the mesos-executor (in the case we launched a container for a
  // task). As a reminder, the mesos-executor exits because it's doing
  // a 'docker wait' on the container using the --override flag of
  // mesos-executor.
  //
  // NOTE: We might not actually have a container or mesos-executor
  // running (which we could check by looking if 'containerId' is a
  // key in 'statuses'). If that is the case then we're doing a
  // destroy because we failed to launch (see defer at bottom of
  // 'launch'). We try and destroy regardless for now, just to be
  // safe.

  // TODO(benh): Retry 'docker rm -f' if it failed but the container
  // still exists (asynchronously).
  docker.kill(containerName(containerId), true)
    .onAny(defer(self(), &Self::_destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::_destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Nothing>& future)
{
  if (!future.isReady()) {
    promises[containerId]->fail(
        "Failed to destroy container: " +
        (future.isFailed() ? future.failure() : "discarded future"));

    destroying.erase(containerId);

    return;
  }

  // It's possible we've been asked to destroy a container that we
  // aren't actually reaping any status because we failed to start the
  // container in the first place (e.g., because we returned a Failure
  // in 'launch' or '_launch'). In this case, we just put a None
  // status in place so that the rest of the destroy workflow
  // completes.
  if (!statuses.contains(containerId)) {
    statuses[containerId] = None();
  }

  statuses[containerId]
    .onAny(defer(self(), &Self::__destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::__destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Option<int> >& status)
{
  containerizer::Termination termination;
  termination.set_killed(killed);
  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }
  termination.set_message(killed ?
                          "Docker task killed" : "Docker process terminated");

  promises[containerId]->set(termination);

  destroying.erase(containerId);
  promises.erase(containerId);
  statuses.erase(containerId);
}


Future<hashset<ContainerID> > DockerContainerizerProcess::containers()
{
  return promises.keys();
}


void DockerContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
    return;
  }

  LOG(INFO) << "Executor for container '" << containerId << "' has exited";

  // The executor has exited so destroy the container.
  destroy(containerId, false);
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
