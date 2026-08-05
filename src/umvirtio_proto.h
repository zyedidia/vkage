/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umvirtio: wire protocol between a UML device shim and a host bridge.
 *
 * This header is duplicated verbatim in the host bridge (as
 * src/umvirtio_proto.h). The two copies must stay byte-identical; every
 * field is fixed-width and little-endian, and the structs are packed.
 * <linux/types.h> is used rather than <stdint.h> because this must compile
 * in three contexts: UML kernel code, UML user-side code (host libc
 * headers), and the host bridge.
 *
 * Topology. UML owns all shared memory: it allocates the vrings and the
 * per-request slot buffers out of its own physmem, which is an fd-backed
 * mapping (see arch/um/kernel/physmem.c). It hands that fd to the host,
 * which maps the whole thing once. A UML kernel virtual address then
 * translates to a host pointer by simple arithmetic:
 *
 *	host_ptr = map_base + (uml_kva - uml_physmem)
 *
 * Polarity is inverted from normal virtio: the host is the *driver* side
 * of each vring and UML is the *device* side (via vringh). That is what
 * lets UML use plain kernel pointers throughout.
 *
 * Descriptor layout is fixed at init and never renegotiated. A queue has
 * 2 * queue_size descriptors; request slot i always uses descriptor 2*i
 * for device-readable data and 2*i+1 for device-writable data. UML fills
 * in every desc->addr once at startup, so the host only ever writes len,
 * flags and next. A request with no outbound data starts its chain at
 * 2*i+1 instead.
 */

#ifndef __UMVIRTIO_PROTO_H__
#define __UMVIRTIO_PROTO_H__

#include <linux/types.h>

#define UMV_MAGIC	0x54564d55	/* "UMVT" */
#define UMV_VERSION	1

#define UMV_MAX_QUEUES	8
#define UMV_MAX_CONFIG	256

/* Bounds on the geometry a shim may ask for. */
#define UMV_MIN_QUEUE_SIZE	4
#define UMV_MAX_QUEUE_SIZE	512
#define UMV_MAX_SLOT_SIZE	(1024 * 1024)

/*
 * Feature set of the umvirtio ring itself, fixed on both sides and never
 * negotiated. This is deliberately separate from the device-type features
 * in struct umv_hello: those are negotiated with the *host kernel* over
 * VDUSE and describe the device (blk flush, net csum, ...), whereas these
 * describe this transport's ring layout. Conflating them would let a
 * host-kernel feature such as EVENT_IDX silently change how this ring is
 * interpreted on one side only.
 *
 * VERSION_1 alone: modern layout, little-endian, no indirect descriptors
 * and no used/avail event suppression.
 */
#define UMV_RING_FEATURES	(1ULL << 32)	/* VIRTIO_F_VERSION_1 */

/*
 * Alignment passed to vring_init()/vring_size() on both sides. Fixed
 * rather than PAGE_SIZE so the two processes cannot disagree.
 */
#define UMV_VRING_ALIGN		4096

enum umv_msg_type {
	UMV_MSG_HELLO	= 1,	/* UML -> host, once, before anything else */
	UMV_MSG_START	= 2,	/* host -> UML, after feature negotiation */
	UMV_MSG_CONFIG	= 3,	/* UML -> host, config space changed */
};

/*
 * UML -> host. Sent once at startup, with these fds attached via
 * SCM_RIGHTS in this exact order:
 *
 *	[0]			physmem fd
 *	[1 .. N]		kick eventfd  (host writes, UML waits)
 *	[N+1 .. 2N]		call eventfd  (UML writes, host waits)
 *
 * where N is num_queues. Everything the host needs to create the VDUSE
 * device is here, so the host holds no device-type-specific knowledge.
 */
struct umv_hello {
	__u32 magic;
	__u32 type;		/* UMV_MSG_HELLO */
	__u32 version;

	__u32 device_id;	/* virtio device id, e.g. VIRTIO_ID_BLOCK */
	__u32 vendor_id;
	__u32 config_size;	/* valid bytes in config[] */
	__u64 features;		/* offered feature bits */

	__u32 num_queues;
	__u32 queue_size;	/* requests in flight per queue; power of two */

	__u64 slot_size;	/* max bytes per direction per request */
	__u64 physmem_size;	/* bytes to map from the physmem fd */
	__u64 uml_physmem;	/* kva corresponding to physmem offset 0 */

	/* Kernel virtual address of each queue's vring. */
	__u64 vring_kva[UMV_MAX_QUEUES];

	__u8 config[UMV_MAX_CONFIG];
} __attribute__((packed));

/*
 * host -> UML. Sent once the host kernel has finished negotiating, so the
 * shim knows which feature bits are live before it touches a descriptor.
 * UML must not process any queue before this arrives.
 */
struct umv_start {
	__u32 magic;
	__u32 type;		/* UMV_MSG_START */
	__u64 features;		/* negotiated subset of umv_hello.features */
} __attribute__((packed));

/*
 * UML -> host. Config space changed underneath us; the host copies this
 * into the VDUSE device and raises a config interrupt.
 */
struct umv_config {
	__u32 magic;
	__u32 type;		/* UMV_MSG_CONFIG */
	__u32 offset;
	__u32 length;
	__u8 data[UMV_MAX_CONFIG];
} __attribute__((packed));

#endif /* __UMVIRTIO_PROTO_H__ */
