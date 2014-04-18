//
// vr4300/pipeline.c: VR4300 processor pipeline.
//
// CEN64: Cycle-Accurate Nintendo 64 Simulator.
// Copyright (C) 2014, Tyler J. Stachecki.
//
// This file is subject to the terms and conditions defined in
// 'LICENSE', which is part of this source code package.
//

#include "common.h"
#include "bus/controller.h"
#include "vr4300/cp0.h"
#include "vr4300/cpu.h"
#include "vr4300/decoder.h"
#include "vr4300/fault.h"
#include "vr4300/opcodes.h"
#include "vr4300/pipeline.h"
#include "vr4300/segment.h"

// Prints out instructions and their virtual address as they are executed.
// Note: These instructions should _may_ be speculative and killed later...
//#define PRINT_EXEC

// Instruction cache stage.
static inline int vr4300_ic_stage(struct vr4300 *vr4300) {
  struct vr4300_rfex_latch *rfex_latch = &vr4300->pipeline.rfex_latch;
  struct vr4300_icrf_latch *icrf_latch = &vr4300->pipeline.icrf_latch;
  const struct segment *segment = icrf_latch->segment;
  uint64_t pc = icrf_latch->pc;
  uint32_t decode_iw;

  icrf_latch->common.pc = pc;

  // Finish decoding instruction in RF.
  decode_iw = rfex_latch->iw &= rfex_latch->iw_mask;
  rfex_latch->opcode = *vr4300_decode_instruction(decode_iw);
  rfex_latch->iw_mask = ~0U;

  // Look up the segment that we're in.
  if ((pc - segment->start) > segment->length) {
    uint32_t cp0_status = vr4300->regs[VR4300_CP0_REGISTER_STATUS];

    if (unlikely((segment = get_segment(pc, cp0_status)) == NULL)) {
      VR4300_IADE(vr4300);
      return 1;
    }

    icrf_latch->segment = segment;
  }

  // We didn't have an IADE, so reset the status vector.
  icrf_latch->common.fault = VR4300_FAULT_NONE;
  icrf_latch->pc += 4;
  return 0;
}

// Register fetch and decode stage.
static inline int vr4300_rf_stage(struct vr4300 *vr4300) {
  const struct vr4300_icrf_latch *icrf_latch = &vr4300->pipeline.icrf_latch;
  struct vr4300_rfex_latch *rfex_latch = &vr4300->pipeline.rfex_latch;
  const struct segment *segment = icrf_latch->segment;

  rfex_latch->common = icrf_latch->common;

  if (!segment->cached) {
    VR4300_UNC(vr4300);
    return 1;
  }

  return 0;
}

// Execution stage.
static inline int vr4300_ex_stage(struct vr4300 *vr4300) {
  const struct vr4300_rfex_latch *rfex_latch = &vr4300->pipeline.rfex_latch;
  const struct vr4300_dcwb_latch *dcwb_latch = &vr4300->pipeline.dcwb_latch;
  struct vr4300_exdc_latch *exdc_latch = &vr4300->pipeline.exdc_latch;
  uint64_t rs_reg, rt_reg, temp;
  uint32_t flags, iw;
  unsigned rs, rt;

  exdc_latch->common = rfex_latch->common;

  flags = rfex_latch->opcode.flags;
  if (exdc_latch->request.type == VR4300_BUS_REQUEST_NONE)
    flags &= ~(OPCODE_INFO_NEEDRS | OPCODE_INFO_NEEDRT);

  iw = rfex_latch->iw;
  rt = GET_RT(iw);
  rs = GET_RS(iw);

  // Check to see if we should hold off execution due to a LDI.
  if (((dcwb_latch->dest == rs) && (flags & OPCODE_INFO_NEEDRS)) ||
    ((dcwb_latch->dest == rt) && (flags & OPCODE_INFO_NEEDRT))) {
    VR4300_LDI(vr4300);
    return 1;
  }

  // No LDI, so we just need to forward results from DC/WB.
  // This is done to preserve RF state and fwd without branching.
  temp = vr4300->regs[dcwb_latch->dest];
  vr4300->regs[dcwb_latch->dest] = dcwb_latch->result;
  vr4300->regs[VR4300_REGISTER_R0] = 0x0000000000000000ULL;

  rs_reg = vr4300->regs[GET_RS(iw)];
  rt_reg = vr4300->regs[GET_RT(iw)];

  vr4300->regs[dcwb_latch->dest] = temp;

  // Finally, execute the instruction.
#ifdef PRINT_EXEC
  fprintf(stderr, "%.16llX: %s\n", (unsigned long long) rfex_latch->common.pc,
    vr4300_opcode_mnemonics[rfex_latch->opcode.id]);
#endif

  exdc_latch->dest = VR4300_REGISTER_R0;
  exdc_latch->request.type = VR4300_BUS_REQUEST_NONE;
  vr4300_function_table[rfex_latch->opcode.id](vr4300, rs_reg, rt_reg);

  return 0;
}

// Data cache fetch stage.
static inline int vr4300_dc_stage(struct vr4300 *vr4300) {
  struct vr4300_exdc_latch *exdc_latch = &vr4300->pipeline.exdc_latch;
  struct vr4300_dcwb_latch *dcwb_latch = &vr4300->pipeline.dcwb_latch;
  const struct segment *segment = exdc_latch->segment;
  uint64_t address = exdc_latch->request.address;

  dcwb_latch->common = exdc_latch->common;
  dcwb_latch->result = exdc_latch->result;
  dcwb_latch->dest = exdc_latch->dest;

  // Look up the segment that we're in.
  if (exdc_latch->request.type != VR4300_BUS_REQUEST_NONE) {
    if ((address - segment->start) > segment->length) {
      uint32_t cp0_status = vr4300->regs[VR4300_CP0_REGISTER_STATUS];

      if (unlikely((segment = get_segment(address, cp0_status)) == NULL)) {
        VR4300_DADE(vr4300);
        return 1;
      }
    }

    exdc_latch->segment = segment;
    exdc_latch->request.address -= segment->offset;

    if (exdc_latch->request.type == VR4300_BUS_REQUEST_READ) {
      VR4300_DCB(vr4300); // TODO/FIXME: Not accurate.
      return 1;
    }

    else {
      assert(exdc_latch->request.type == VR4300_BUS_REQUEST_WRITE &&
        "Unsupported DC stage request type.");

      // TODO/FIXME: Not accurate.
      bus_write_word(vr4300->bus, exdc_latch->request.address,
        exdc_latch->request.word, exdc_latch->request.dqm);
    }
  }

  return 0;
}

// Writeback stage.
static inline int vr4300_wb_stage(struct vr4300 *vr4300) {
  const struct vr4300_dcwb_latch *dcwb_latch = &vr4300->pipeline.dcwb_latch;

  if (dcwb_latch->common.fault != VR4300_FAULT_NONE)
    return 0;

  vr4300->regs[dcwb_latch->dest] = dcwb_latch->result;
  vr4300->regs[VR4300_REGISTER_R0] = 0x0000000000000000ULL;
  return 0;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
static void vr4300_cycle_slow_wb(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;
  struct vr4300_dcwb_latch *dcwb_latch = &pipeline->dcwb_latch;
  struct vr4300_exdc_latch *exdc_latch = &pipeline->exdc_latch;
  struct vr4300_rfex_latch *rfex_latch = &pipeline->rfex_latch;
  struct vr4300_icrf_latch *icrf_latch = &pipeline->icrf_latch;

  // If we haven't had exceptions for at least a
  // full pipeline's length, switch back to fast mode.
  if (pipeline->exception_history++ > 4)
    pipeline->fault_present = false;

  if (dcwb_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_wb_stage(vr4300))
      return;
  }

  else
    dcwb_latch->common = exdc_latch->common;

  if (exdc_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_dc_stage(vr4300))
      return;
  }

  else
    exdc_latch->common = rfex_latch->common;

  if (rfex_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_ex_stage(vr4300))
      return;
  }

  else
    rfex_latch->common = icrf_latch->common;

  if (icrf_latch->common.fault == VR4300_FAULT_NONE)
    if (vr4300_rf_stage(vr4300))
      return;

  if (vr4300_ic_stage(vr4300))
    return;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
//
// Starts from DC stage (WB resolved an interlock).
static void vr4300_cycle_slow_dc(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;
  struct vr4300_exdc_latch *exdc_latch = &pipeline->exdc_latch;
  struct vr4300_rfex_latch *rfex_latch = &pipeline->rfex_latch;
  struct vr4300_icrf_latch *icrf_latch = &pipeline->icrf_latch;

  if (exdc_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_dc_stage(vr4300))
      return;
  }

  else
    exdc_latch->common = rfex_latch->common;

  if (rfex_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_ex_stage(vr4300))
      return;
  }

  else
    rfex_latch->common = icrf_latch->common;

  if (icrf_latch->common.fault == VR4300_FAULT_NONE)
    if (vr4300_rf_stage(vr4300))
      return;

  if (vr4300_ic_stage(vr4300))
    return;

  pipeline->skip_stages = 0;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
//
// Starts from EX stage (DC resolved an interlock).
static void vr4300_cycle_slow_ex(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;
  struct vr4300_rfex_latch *rfex_latch = &pipeline->rfex_latch;
  struct vr4300_icrf_latch *icrf_latch = &pipeline->icrf_latch;

  if (rfex_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_ex_stage(vr4300))
      return;
  }

  else
    rfex_latch->common = icrf_latch->common;

  if (icrf_latch->common.fault == VR4300_FAULT_NONE)
    if (vr4300_rf_stage(vr4300))
      return;

  if (vr4300_ic_stage(vr4300))
    return;

  pipeline->skip_stages = 0;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
//
// Starts from EX stage (DC resolved an interlock).
// Fixes up the DC/WB latches after memory reads.
static void vr4300_cycle_slow_ex_fixdc(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;
  struct vr4300_icrf_latch *icrf_latch = &pipeline->icrf_latch;
  struct vr4300_rfex_latch *rfex_latch = &pipeline->rfex_latch;
  struct vr4300_exdc_latch *exdc_latch = &pipeline->exdc_latch;
  struct vr4300_dcwb_latch *dcwb_latch = &pipeline->dcwb_latch;
  struct vr4300_bus_request *request = &exdc_latch->request;

  int maskshift = request->size << 3;
  int datashift = (8 - request->size) << 3;
  uint64_t data = (uint32_t) request->word;
  int64_t sdata = (int32_t) request->word;
  int64_t mask = exdc_latch->result;

  // Shall we sign extend?
  mask = (mask >> maskshift) << maskshift;
  sdata = (sdata << datashift) >> datashift;
  data = (data << datashift) >> datashift;
  data = (sdata & mask) | data;
  dcwb_latch->result = data;

  // Continue with the rest of the pipeline.
  if (rfex_latch->common.fault == VR4300_FAULT_NONE) {
    if (vr4300_ex_stage(vr4300))
      return;
  }

  else
    rfex_latch->common = icrf_latch->common;

  if (icrf_latch->common.fault == VR4300_FAULT_NONE)
    if (vr4300_rf_stage(vr4300))
      return;

  if (vr4300_ic_stage(vr4300))
    return;

  pipeline->skip_stages = 0;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
//
// Starts from RF stage (EX resolved an interlock).
static void vr4300_cycle_slow_rf(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;
  struct vr4300_icrf_latch *icrf_latch = &pipeline->icrf_latch;

  if (icrf_latch->common.fault == VR4300_FAULT_NONE)
    if (vr4300_rf_stage(vr4300))
      return;

  if (vr4300_ic_stage(vr4300))
    return;

  pipeline->skip_stages = 0;
}

// Advances the processor pipeline by one pclock.
// May have exceptions, so check for aborted stages.
//
// Starts from IC stage (RF resolved an interlock).
static void vr4300_cycle_slow_ic(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;

  if (vr4300_ic_stage(vr4300))
    return;

  pipeline->skip_stages = 0;
}

// LUT of stages for fault handling.
typedef void (*pipeline_function)(struct vr4300 *vr4300);
static const pipeline_function pipeline_function_lut[6] = {
  vr4300_cycle_slow_wb,
  vr4300_cycle_slow_dc,
  vr4300_cycle_slow_ex,
  vr4300_cycle_slow_rf,
  vr4300_cycle_slow_ic,
  vr4300_cycle_slow_ex_fixdc,
};

// Advances the processor pipeline by one pclock.
void vr4300_cycle(struct vr4300 *vr4300) {
  struct vr4300_pipeline *pipeline = &vr4300->pipeline;

  // We're stalling for an interlock,
  // or we just took an exception...
  if (pipeline->cycles_to_stall > 0) {
    pipeline->cycles_to_stall--;
    return;
  }

  // The reset exception has a very high priority and will abort basically
  // anything that's active, even if we have an interlock or something that's
  // current active. Thus, we check for it here and handle it early.
  if (unlikely(vr4300->signals & VR4300_SIGNAL_COLDRESET))
    VR4300_RST(vr4300);

  // Ordinarily, we would need to check every pipeline stage to see if it is
  // aborted, and conditionally not execute it. Since faults are rare, we'll
  // only bother checking for aborted stages when we know they can be present.
  if (pipeline->fault_present || pipeline->skip_stages) {
    pipeline_function_lut[pipeline->skip_stages](vr4300);
    return;
  }

  if (vr4300_wb_stage(vr4300))
    return;

  if (vr4300_dc_stage(vr4300))
    return;

  if (vr4300_ex_stage(vr4300))
    return;

  if (vr4300_rf_stage(vr4300))
    return;

  if (vr4300_ic_stage(vr4300))
    return;
}

// Initializes the pipeline with default values.
void vr4300_pipeline_init(struct vr4300_pipeline *pipeline) {
  memset(pipeline, 0, sizeof(*pipeline));

  pipeline->icrf_latch.segment = get_default_segment();
  pipeline->exdc_latch.segment = get_default_segment();
}

