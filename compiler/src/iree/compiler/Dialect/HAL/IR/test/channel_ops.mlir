// RUN: iree-opt --split-input-file %s | iree-opt --split-input-file | FileCheck %s

// CHECK-LABEL: @channel_create
//  CHECK-SAME: (%[[DEVICE:.+]]: !hal.device, %[[AFFINITY:.+]]: i64, %[[RANK:.+]]: i32, %[[COUNT:.+]]: i32)
func.func @channel_create(%device: !hal.device, %affinity: i64, %rank: i32, %count: i32) {
  //      CHECK: %channel = hal.channel.create
  // CHECK-SAME:   device(%[[DEVICE]] : !hal.device)
  // CHECK-SAME:   affinity(%[[AFFINITY]])
  // CHECK-SAME:   rank(%[[RANK]])
  // CHECK-SAME:   count(%[[COUNT]]) : !hal.channel
  %channel = hal.channel.create device(%device : !hal.device)
                              affinity(%affinity)
                                  rank(%rank)
                                 count(%count) : !hal.channel
  return
}

// -----

// CHECK-LABEL: @channel_rank_and_count
// CHECK-SAME: (%[[CHANNEL:.+]]: !hal.channel)
func.func @channel_rank_and_count(%channel: !hal.channel) -> (i32, i32) {
  // CHECK: = hal.channel.rank_and_count<%[[CHANNEL]] : !hal.channel> : i32, i32
  %rank, %count = hal.channel.rank_and_count<%channel : !hal.channel> : i32, i32
  return %rank, %count : i32, i32
}
