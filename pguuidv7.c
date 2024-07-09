
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>


#include "utils/uuid.h"

#include "fmgr.h"

#include "port.h"
#include "pg_config.h"

#include <openssl/rand.h>

PG_MODULE_MAGIC;


/* SQL function: gen_random_uuid() returns uuid */
PG_FUNCTION_INFO_V1(uuidv7);

/* pg_uuid_t is declared to be struct pg_uuid_t in uuid.h */
typedef struct pg_uuid_t_v7
{
	unsigned char data[UUID_LEN];
} pg_uuid_t_v7;



/*
 * Generate UUID version 7.
 *
 * Following description is taken from RFC draft and slightly extended to
 * reflect implementation specific choices.
 *
 * "Fixed-Length Dedicated Counter Bits" (Method 1) can be implemented with
 * arbitrary size of a counter. We choose size 18 to reuse all space of bytes
 * that are touched by ver and var fields + rand_a bytes between them.
 * Whenever timestamp unix_ts_ms is moving forward, this counter bits are
 * reinitialized. Reinilialization always sets most significant bit to 0, other
 * bits are initialized with random numbers. This gives as approximately 192K
 * UUIDs within one millisecond without overflow. This ougth to be enough for
 * most practical purposes. Whenever counter overflow happens, this overflow is
 * translated to increment of unix_ts_ms. So generation of UUIDs ate rate
 * higher than 128MHz might lead to using timestamps ahead of time.
 *
 * We're not using the "Replace Left-Most Random Bits with Increased Clock
 * Precision" method Section 6.2 (Method 3), because of portability concerns.
 * It's unclear if all supported platforms can provide reliable microsocond
 * precision time.
 *
 * All UUID generator state is backend-local. For UUIDs generated in one
 * backend we guarantee monotonicity. UUIDs generated on different backends
 * will be mostly monotonic if they are generated at frequences less than 1KHz,
 * but this monotonicity is not strictly guaranteed. UUIDs generated on
 * different nodes are mostly monotonic with regards to possible clock drift.
 * Uniqueness of UUIDs generated at the same timestamp across different
 * backends and/or nodes is guaranteed by using random bits.
 */
Datum
uuidv7(PG_FUNCTION_ARGS)
{
	static uint32 sequence_counter;
	static uint64 previous_timestamp = 0;

	pg_uuid_t_v7  *uuid = palloc(UUID_LEN);
	uint64	tms;
	struct timeval tp;
	bool		time_tick_forward;

	gettimeofday(&tp, NULL);
	tms = ((uint64) tp.tv_sec) * 1000 + (tp.tv_usec) / 1000;
	/* time from clock is protected from backward leaps */
	time_tick_forward = (tms > previous_timestamp);

	if (time_tick_forward)
	{
		/* fill everything after the timestamp with random bytes */
		if (!pg_strong_random(&uuid->data[6], UUID_LEN - 6))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random values")));

		/*
		 * Left-most counter bit is initialized as zero for the sole purpose
		 * of guarding against counter rollovers. See section "Fixed-Length
		 * Dedicated Counter Seeding"
		 * https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis#monotonicity_counters
		 */
		uuid->data[6] = (uuid->data[6] & 0xf7);

		/* read randomly initialized bits of counter */
		sequence_counter = ((uint32) uuid->data[8] & 0x3f) +
			(((uint32) uuid->data[7]) << 6) +
			(((uint32) uuid->data[6] & 0x0f) << 14);

		previous_timestamp = tms;
	}
	else
	{
		/*
		 * Time did not advance from the previous generation, we must
		 * increment counter
		 */
		++sequence_counter;
		if (sequence_counter > 0x3ffff)
		{
			/* We only have 18-bit counter */
			sequence_counter = 0;
			previous_timestamp++;
		}

		/* protection from leap backward */
		tms = previous_timestamp;

		/* fill everything after the timestamp and counter with random bytes */
		if (!pg_strong_random(&uuid->data[9], UUID_LEN - 9))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random values")));

		/* most significant 4 bits of 18-bit counter */
		uuid->data[6] = (unsigned char) (sequence_counter >> 14);
		/* next 8 bits */
		uuid->data[7] = (unsigned char) (sequence_counter >> 6);
		/* least significant 6 bits */
		uuid->data[8] = (unsigned char) (sequence_counter);
	}

	/* Fill in time part */
	uuid->data[0] = (unsigned char) (tms >> 40);
	uuid->data[1] = (unsigned char) (tms >> 32);
	uuid->data[2] = (unsigned char) (tms >> 24);
	uuid->data[3] = (unsigned char) (tms >> 16);
	uuid->data[4] = (unsigned char) (tms >> 8);
	uuid->data[5] = (unsigned char) tms;

	/*
	 * Set magic numbers for a "version 7" (pseudorandom) UUID, see
	 * https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis
	 */
	/* set version field, top four bits are 0, 1, 1, 1 */
	uuid->data[6] = (uuid->data[6] & 0x0f) | 0x70;
	/* set variant field, top two bits are 1, 0 */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80;

	PG_RETURN_UUID_P((pg_uuid_t *) uuid);
}

#define GREGORIAN_EPOCH_JDATE  2299161 /* == date2j(1582,10,15) */
