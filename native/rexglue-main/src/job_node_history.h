#pragma once
#include <cstdint>
enum Ao4JobStage { PopDone, PushEnter, PushDone, ChainEnter, CallbackEnter,
                  CallbackDone, CancelEnter, CancelDone, EnqueueEnter, EnqueueDone, DispatchEnter, ClaimDone, QueueDetach, QueuePublish };
void Ao4RecordJob(Ao4JobStage stage, uint32_t node, uint32_t queue,
                 uint32_t caller, uint32_t extra, uint8_t* base, uint64_t context = 0,
                 uint64_t replaced = 0, uint64_t written = 0);
