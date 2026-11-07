/*
 * pldm.h - PLDM over MCTP (DMTF DSP0240 base, DSP0248 platform monitoring,
 * DSP0267 firmware update) - the subset a small management controller needs.
 *
 * PLDM message inside an MCTP message:
 *   [0] MCTP type 0x01
 *   [1] Rq(7) D(6) rsvd(5) InstanceID(4:0)
 *   [2] HdrVer(7:6)=0 | PLDMType(5:0)
 *   [3] command
 *   [4] (responses) completion code, then payload
 *
 * Supported:
 *   Type 0  Base     : GetTID, GetPLDMVersion, GetPLDMTypes, GetPLDMCommands
 *   Type 2  Platform : GetSensorReading (numeric sensors 1 = temp, 2 = volt)
 *   Type 5  FW update: Firmware Device side - RequestUpdate, PassComponentTable,
 *                      UpdateComponent, ActivateFirmware, GetStatus,
 *                      CancelUpdate; issues RequestFirmwareData,
 *                      TransferComplete, VerifyComplete, ApplyComplete
 *   Type 63 OEM      : error-log access, link statistics, test hooks
 *
 * Not implemented (and needed before an OpenBMC pldmd can manage this
 * device): the PDR repository (GetPDR) that describes sensors, event
 * generation (PlatformEventMessage), multi-part GetPLDMVersion.
 */
#ifndef PMC_PLDM_H
#define PMC_PLDM_H

#include "pmc/mctp.h"
#include "pmc/monitor.h"
#include "pmc/raslog.h"

#define PLDM_TYPE_BASE      0x00
#define PLDM_TYPE_PLATFORM  0x02
#define PLDM_TYPE_FWUP      0x05
#define PLDM_TYPE_OEM       0x3F

/* completion codes (DSP0240) */
#define PLDM_SUCCESS                 0x00
#define PLDM_ERROR                   0x01
#define PLDM_ERROR_INVALID_DATA      0x02
#define PLDM_ERROR_INVALID_LENGTH    0x03
#define PLDM_ERROR_NOT_READY         0x04
#define PLDM_ERROR_UNSUPPORTED_CMD   0x05
#define PLDM_ERROR_INVALID_TYPE      0x20

/* base commands */
#define PLDM_GET_TID          0x02
#define PLDM_GET_VERSION      0x03
#define PLDM_GET_TYPES        0x04
#define PLDM_GET_COMMANDS     0x05
/* platform */
#define PLDM_GET_SENSOR_READING 0x11
/* OEM (this project) */
#define PLDM_OEM_ERRLOG_COUNT  0x01
#define PLDM_OEM_ERRLOG_GET    0x02
#define PLDM_OEM_ERRLOG_INJECT 0x03   /* only with PMC_ALLOW_TEST_INJECT */
#define PLDM_OEM_LINK_STATS    0x04

/* ---------------- firmware update (DSP0267) ---------------- */
#define FWUP_REQUEST_UPDATE        0x10
#define FWUP_PASS_COMPONENT_TABLE  0x13
#define FWUP_UPDATE_COMPONENT      0x14
#define FWUP_REQUEST_FW_DATA       0x15
#define FWUP_TRANSFER_COMPLETE     0x16
#define FWUP_VERIFY_COMPLETE       0x17
#define FWUP_APPLY_COMPLETE        0x18
#define FWUP_ACTIVATE_FIRMWARE     0x1A
#define FWUP_GET_STATUS            0x1B
#define FWUP_CANCEL_UPDATE         0x1D

#define FWUP_CC_NOT_IN_UPDATE_MODE      0x80
#define FWUP_CC_ALREADY_IN_UPDATE_MODE  0x81
#define FWUP_CC_DATA_OUT_OF_RANGE       0x82
#define FWUP_CC_INVALID_TRANSFER_LENGTH 0x83
#define FWUP_CC_INVALID_STATE           0x84

enum pmc_fwup_state {
	FD_IDLE = 0, FD_LEARN_COMPONENTS = 1, FD_READY_XFER = 2, FD_DOWNLOAD = 3,
	FD_VERIFY = 4, FD_APPLY = 5, FD_ACTIVATE = 6
};

/* Where the image goes. Zephyr: flash_img into MCUboot's secondary slot,
 * then boot_request_upgrade(). Simulator: a file + SHA-256 check. */
typedef struct {
	pmc_err_t (*begin)(void *ctx, uint32_t size);
	pmc_err_t (*write)(void *ctx, uint32_t off, const uint8_t *data, size_t len);
	pmc_err_t (*verify)(void *ctx);     /* signature / integrity check */
	pmc_err_t (*apply)(void *ctx);      /* mark slot for test boot      */
	pmc_err_t (*activate)(void *ctx);   /* schedule reboot into it      */
	void *ctx;
} pmc_fw_backend;

typedef struct {
	enum pmc_fwup_state state, prev_state;
	uint32_t max_xfer, image_size, offset, chunk;
	uint8_t ua_eid;
	uint8_t pending_cmd;      /* request we sent and await a response for */
	uint8_t pending_iid;
	uint8_t pending_tag;
	uint8_t reason;           /* last failure reason (GetStatus) */
	pmc_fw_backend be;
} pmc_fwup;

typedef struct {
	pmc_mctp *mctp;
	pmc_monitor *mon;
	pmc_raslog *log;
	pmc_mctp_deframer *link;  /* for link statistics, may be NULL */
	pmc_fwup fw;
	uint8_t tid;
	uint8_t next_iid;
} pmc_pldm;

void pmc_pldm_init(pmc_pldm *p, pmc_mctp *mctp, pmc_monitor *mon, pmc_raslog *log,
		   const pmc_fw_backend *fw);

/* Entry point from the MCTP rx callback for message type 0x01. */
void pmc_pldm_handle(pmc_pldm *p, uint8_t src, uint8_t tag, int tag_owner,
		     const uint8_t *msg, size_t len);

/* Single dispatcher for MCTP messages (control + PLDM). Set as the
 * pmc_mctp rx callback with user = pmc_pldm*. */
void pmc_rx_dispatch(pmc_mctp *m, uint8_t src, uint8_t tag, int tag_owner,
		     const uint8_t *msg, size_t len);

#endif
