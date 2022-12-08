#!/bin/bash

# Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# This is the series of commands run on the a VM from a fresh image in order to
# set up the disk to be used as a boot image. This script must be run as root.

set -o verbose   # Print all command lines literally as they are read
set -o xtrace    # Print all commands after they are expanded
set -o errexit   # Exit if any command fails
set -o errtrace  # make ERR trap inherit
set -o pipefail  # return error if any part of a pipe errors
set -o nounset   # error if an undefined variable is used


# Copied from build_tools/github_actions/runner/config/functions.sh
get_metadata() {
  local url="http://metadata.google.internal/computeMetadata/v1/${1}"
  ret=0
  curl "${url}" \
    --silent --fail --show-error \
    --header "Metadata-Flavor: Google" || ret=$?
  if [[ "${ret}" != 0 ]]; then
    echo "Failed fetching ${url}" >&2
    return "${ret}"
  fi
}

get_attribute() {
  get_metadata "instance/attributes/${1}"
}

RUNNER_TYPE="$(get_attribute github-runner-type)"
GCLOUD_VERSION=402.0.0
GCLOUD_ARCHIVE_DIGEST=a9902b57d4cba2ebb76d7354570813d3d8199c36b95a1111a1b7fea013beaaf9

function save_exit_code() {
  local exit_code="$?"
  echo "${exit_code}" > /startup-exit.txt
  trap - EXIT
  exit "${exit_code}"
}

trap save_exit_code EXIT INT TERM

function apt_maybe_purge() {
  # Remove and purge packages if they are installed and don't error if they're
  # not or if they're not findable in the ppa.
  local -a to_remove=()
  for pkg in "$@"; do
    ret=0
    if dpkg --status $pkg &> /dev/null; then
      to_remove+=("${pkg}")
    fi
  done
  if (( "${#to_remove[@]}" != 0 )); then
    apt-get remove --purge --autoremove "${to_remove[@]}"
  fi
}

function nice_curl() {
  curl --silent --fail --show-error --location "$@"
}

function startup() {
  # Shut down in 5 hours. Makes sure this instance doesn't hang around forever
  # if setup fails. Someone can cancel the shutdown with `shutdown -c`.
  nohup shutdown -h +300 &
  cd /

  ########################### Create the runner user ###########################

  # GCE "helpfully" creates users for apparently any account that has ever
  # logged in on any VM. Delete it if it's there.
  userdel --force --remove runner || true
  adduser --system --group "runner"
  groupadd docker
  usermod --append --groups docker runner
  usermod --append --groups sudo runner
  groups runner # Print out the groups of runner to verify this worked
  groups runner | grep docker || (echo "Failed to add runner user to docker group" && exit 1)
  groups runner | grep sudo || (echo "Failed to add runner user to sudo group" && exit 1)

  echo "enabling passwordless sudo for runner user"
  echo "runner ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/99-runner

  # Confirm that worked
  runuser --user runner -- sudo echo "runner user has passwordless sudo"

  #################################### Apt #####################################
  # Disable apt prompts
  export DEBIAN_FRONTEND="noninteractive"

  # Disable automatic updates and upgrades. These are ephemeral machines. We don't
  # want the latency or inconsistency of automatic updatees.
  systemctl stop apt-daily.timer
  systemctl disable apt-daily.timer
  systemctl disable apt-daily.service
  systemctl stop apt-daily-upgrade.timer
  systemctl disable apt-daily-upgrade.timer
  systemctl disable apt-daily-upgrade.service

  # Don't install documentation (except copyrights) since this is a CI system.
  cat > /etc/dpkg/dpkg.cfg.d/99-github-actions <<EOF
force-all
no-pager
# don't install docs
path-exclude /usr/share/doc/*
path-exclude /usr/share/man/*
path-exclude /usr/share/groff/*
path-exclude /usr/share/info/*
# keep copyright files for legal reasons
path-include /usr/share/doc/*/copyright
EOF

  # Provide default apt options like --assume-yes and --quiet since this is
  # designed to run on CI.
  cat > /etc/apt/apt.conf.d/99-github-actions <<EOF
APT {
  Install-Recommends "false";
  HideAutoRemove "true";
}
Aptitude {
  CmdLine {
    Assume-Yes "true";
  }
}
Acquire {
  Retries "5";
}
DPkg {
  Use-Pty "0";
  Options {
    "--force-confdef";
    "--force-confnew";
    "--force-confold";
  }
}
Quiet "2";
EOF

  # We install these common deps. This is a subset of what's installed on the
  # GitHub managed runners. All our heavy stuff is Dockerized, so basically just
  # some utilities.
  local apt_packages=(
    apt-transport-https
    aria2
    ca-certificates
    curl
    git
    gnupg2
    lsb-release
    software-properties-common
    # Useful for working with JSON, which is used quite a bit in GitHub actions.
    jq
    # We need gcc, libc, make, etc for Cuda install
    build-essential
  )

  # Install apt-fast for parallel apt package installation.
  add-apt-repository -y ppa:apt-fast/stable
  apt-get update
  apt-get install apt-fast
  apt-get upgrade
  apt-get dist-upgrade
  apt-get full-upgrade
  apt-get install "${apt_packages[@]}"

  ############################## Fix gcloud Installation Snap ###############################

  # Snap literally won't let you disable automatic updates. The only thing
  # that's installed through snap here is the gcloud CLI, which we definitely
  # don't want automatically updating (beyond our general desire to not
  # automatically update on ephemeral machines). So we just delete snap entirely
  # and install the CLI via apt (above)
  systemctl stop snapd
  apt_maybe_purge snapd gnome-software-plugin-snap
  rm -rf /home/*/snap
  rm -rf /root/snap

  nice_curl \
      https://packages.cloud.google.com/apt/doc/apt-key.gpg \
      | gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg
  echo \
      "deb [arch=amd64 signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" \
      > /etc/apt/sources.list.d/google-cloud-sdk.list
  apt-get update && apt-get install google-cloud-cli

  # This setting is now enabled by default. It sounds great, but unfortunately
  # doing such an upload requires *delete* permissions on the bucket, which we
  # deliberately do not give runners. For the life of me, I could not figure out
  # how to use `gcloud config set` (the "proper" way to set properties) to work
  # on the global properties.
  cat <<EOF >> /usr/lib/google-cloud-sdk/properties
[storage]
parallel_composite_upload_enabled = False
EOF

  ############################### Install Docker ###############################

  # Remove Docker stuff that may already be installed by all its various names
  apt_maybe_purge containerd docker docker-engine docker.io moby-engine moby-cli runc

  # Install the latest Docker
  nice_curl \
    https://download.docker.com/linux/ubuntu/gpg \
    | gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg
  echo \
    "deb [arch=amd64 signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
    > /etc/apt/sources.list.d/docker.list
  apt-get update
  apt-get install docker-ce docker-ce-cli containerd.io

  # Enable docker.service.
  sudo systemctl enable docker.service
  sudo systemctl start docker.service
  sudo systemctl enable containerd.service
  sudo systemctl start containerd.service

  # Docker daemon takes time to come up after installing.
  for i in $(seq 1 30); do
    if docker info; then
      break
    fi
  done

  # Make sure the runner user can use docker
  runuser --user runner -- docker ps

  #################################### GPU #####################################

  if [[ "${RUNNER_TYPE^^}" == GPU ]]; then
    local script_dir="$(mktemp --directory --tmpdir scripts.XXX)"

    nice_curl \
      --remote-name-all \
      --output-dir "${script_dir}" \
      https://raw.githubusercontent.com/iree-org/iree/main/build_tools/scripts/check_vulkan.sh \
      https://raw.githubusercontent.com/iree-org/iree/main/build_tools/scripts/check_cuda.sh

    chmod +x "${script_dir}/check_vulkan.sh" "${script_dir}/check_cuda.sh"

    # Doing these all in one command fails, probably because there's a dependency
    # between them and apt-fast makes it happen in parallel. Also, it turns out
    # that the Vulkan ICD is in libnvidia-gl for some reason.
    apt-get install nvidia-headless-515
    apt-get install libnvidia-gl-515-server nvidia-utils-515-server vulkan-tools
    "${script_dir}/check_cuda.sh"
    "${script_dir}/check_vulkan.sh"


    # Nvidia container toolkit: https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/overview.html
    local distribution="$(source /etc/os-release; echo "${ID}${VERSION_ID}")"
    nice_curl \
        https://nvidia.github.io/libnvidia-container/gpgkey \
        | gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
    nice_curl \
        "https://nvidia.github.io/libnvidia-container/${distribution}/libnvidia-container.list" | \
        sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
        > /etc/apt/sources.list.d/nvidia-container-toolkit.list

    apt-get update
    apt-get install nvidia-docker2
    systemctl restart docker

    # Check GPU usage with Vulkan and Cuda work
    function check_docker() {
      local image="$1"
      docker run --rm --gpus all --env NVIDIA_DRIVER_CAPABILITIES=all \
          --mount="type=bind,source=${script_dir},dst=${script_dir},readonly" \
          "${image}" \
          bash -c "${script_dir}/check_cuda.sh && ${script_dir}/check_vulkan.sh"
    }

    check_docker gcr.io/iree-oss/nvidia@sha256:b0751e9f2fcb104d9d3d56fab6c6e79405bdcd2e503e53f2bf4f2b66d13cd88b
    check_docker gcr.io/iree-oss/frontends-nvidia@sha256:3e3965b6b9e431be296900440e5bc66ed46a8f6d6fa80ead7c7a75866f3bd416

    # Remove the docker images we've fetched. We might want to pre-fetch Docker
    # images into the VM image, but that should be a separate decision.
    docker system prune --force --all
  fi

  ################################### Cleanup ##################################

  apt-get clean
  rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
  rm -rf /var/lib/dhcp/*

  # Delete unnecessary log files
  find /var/log -type f -regex ".*\.gz$" -delete
  find /var/log -type f -regex ".*\.[0-9]$" -delete

  # Clear all journal files
  journalctl --rotate --vacuum-time=1s

  # And clear others
  find /var/log/ -type f -exec truncate -s 0 {} \;

  echo "Disk usage after setup"
  df -h /

  echo "Setup complete"
}

time startup 2>&1 | tee /startup.log
