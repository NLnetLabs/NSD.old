/*
 * nsd-xfer.c -- nsd-xfer(8).
 *
 * Copyright (c) 2001-2004, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "dname.h"
#include "dns.h"
#include "packet.h"
#include "query.h"
#include "rdata.h"
#include "region-allocator.h"
#include "tsig.h"
#include "tsig-openssl.h"
#include "util.h"
#include "zonec.h"


/*
 * Number of seconds to wait when recieving no data from the remote
 * server.
 */
#define MAX_WAITING_TIME TCP_TIMEOUT


/*
 * Exit codes are based on named-xfer for now.  See ns_defs.h in
 * bind8.
 */
enum nsd_xfer_exit_codes
{
	XFER_UPTODATE = 0,
	XFER_SUCCESS  = 1,
	XFER_FAIL     = 3
};

struct axfr_state
{
	int verbose;
	size_t packets_received;
	size_t bytes_received;
	
	int s;			/* AXFR socket.  */
	query_type *q;		/* Query buffer.  */
	uint16_t query_id;	/* AXFR query ID.  */
	tsig_record_type *tsig;	/* TSIG data.  */

	int first_transfer;	 /* First transfer of this zone.  */
	uint32_t last_serial;    /* Otherwise the last serial.  */
	uint32_t zone_serial;	 /* And the new zone serial.  */
	const dname_type *zone;	 /* Zone name.  */
	
	int    done;		/* AXFR is complete.  */
	size_t rr_count;	/* Number of RRs received so far.  */

	/*
	 * Region used to allocate data needed to process a single RR.
	 */
	region_type *rr_region;

	/*
	 * Region used to store owner and origin of previous RR (used
	 * for pretty printing of zone data).
	 */
	region_type *previous_owner_region;
	const dname_type *previous_owner;
	const dname_type *previous_owner_origin;
};
typedef struct axfr_state axfr_state_type;

static sig_atomic_t timeout_flag = 0;
static void to_alarm(int sig);		/* our alarm() signal handler */

extern char *optarg;
extern int optind;

static uint16_t init_query(query_type *q,
			   const dname_type *dname,
			   uint16_t type,
			   uint16_t klass,
			   tsig_record_type *tsig);

/*
 * Log an error message and exit.
 */
static void error(const char *format, ...) ATTR_FORMAT(printf, 1, 2);
static void
error(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_vmsg(LOG_ERR, format, args);
	va_end(args);
	exit(XFER_FAIL);
}


/*
 * Log a warning message.
 */
static void warning(const char *format, ...) ATTR_FORMAT(printf, 1, 2);
static void
warning(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_vmsg(LOG_WARNING, format, args);
	va_end(args);
}


/*
 * Display usage information and exit.
 */
static void
usage (void)
{
	fprintf(stderr,
		"Usage: nsd-xfer [OPTION]... -z zone -f file server...\n"
		"NSD AXFR client.\n\nSupported options:\n"
		"  -4           Only use IPv4 connections.\n"
		"  -6           Only use IPv6 connections.\n"
		"  -f file      Output zone file name.\n"
		"  -p port      The port to connect to.\n"
		"  -s serial    The current zone serial.\n"
		"  -T tsiginfo  The TSIG key file name.  The file is removed "
		"after reading the\n               key.\n"
		"  -v           Verbose output.\n");
	fprintf(stderr,
		"  -z zone      Specify the name of the zone to transfer.\n"
		"  server       The name or IP address of the master server.\n"
		"\nReport bugs to <%s>.\n", PACKAGE_BUGREPORT);
	exit(XFER_FAIL);
}


/*
 * Signal handler for timeouts (SIGALRM). This function is called when
 * the alarm() value that was set counts down to zero.  This indicates
 * that we haven't received a response from the server.
 *
 * All we do is set a flag and return from the signal handler. The
 * occurrence of the signal interrupts the read() system call (errno
 * == EINTR) above, and we then check the timeout_flag flag.
 */
static void
to_alarm(int ATTR_UNUSED(sig))
{
	timeout_flag = 1;
}

static void
cleanup_addrinfo(void *data)
{
	freeaddrinfo((struct addrinfo *) data);
}

/*
 * Read a line from IN.  If successful, the line is stripped of
 * leading and trailing whitespace and non-zero is returned.
 */
static int
read_line(FILE *in, char *line, size_t size)
{
	if (!fgets(line, size, in)) {
		return 0;
	} else {
		strip_string(line);
		return 1;
	}
}

static tsig_key_type *
read_tsig_key_data(region_type *region, FILE *in, int default_family)
{
	char line[4000];
	tsig_key_type *key = (tsig_key_type *) region_alloc(
		region, sizeof(tsig_key_type));
	struct addrinfo hints;
	int gai_rc;
	int size;
	uint8_t data[4000];
	
	if (!read_line(in, line, sizeof(line))) {
		error("failed to read TSIG key server address: '%s'",
		      strerror(errno));
		return NULL;
	}
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags |= AI_NUMERICHOST;
	hints.ai_family = default_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	gai_rc = getaddrinfo(line, NULL, &hints, &key->server);
	if (gai_rc) {
		error("cannot parse address '%s': %s",
		      line,
		      gai_strerror(gai_rc));
		return NULL;
	}

	region_add_cleanup(region, cleanup_addrinfo, key->server);
	
	if (!read_line(in, line, sizeof(line))) {
		error("failed to read TSIG key name: '%s'", strerror(errno));
		return NULL;
	}
	
	key->name = dname_parse(region, line);
	if (!key->name) {
		error("failed to parse TSIG key name '%s'", line);
		return NULL;
	}

	if (!read_line(in, line, sizeof(line))) {
		error("failed to read TSIG key type: '%s'", strerror(errno));
		return NULL;
	}

	if (!read_line(in, line, sizeof(line))) {
		error("failed to read TSIG key data: '%s'\n", strerror(errno));
		return NULL;
	}
	
	size = b64_pton(line, data, sizeof(data));
	if (size == -1) {
		error("failed to parse TSIG key data");
		return NULL;
	}

	key->size = size;
	key->data = (uint8_t *) region_alloc_init(region, data, key->size);

	return key;
}

/*
 * Read the TSIG key from a .tsiginfo file and remove the file.
 */
static tsig_key_type *
read_tsig_key(region_type *region,
	      const char *tsiginfo_filename,
	      int default_family)
{
	FILE *in;
	tsig_key_type *key;
	
	in = fopen(tsiginfo_filename, "r");
	if (!in) {
		error("failed to open %s: %s",
		      tsiginfo_filename,
		      strerror(errno));
		return NULL;
	}

	key = read_tsig_key_data(region, in, default_family);

	fclose(in);
	
	if (unlink(tsiginfo_filename) == -1) {
		warning("failed to remove %s: %s",
			tsiginfo_filename,
			strerror(errno));
	}

	return key;
}

/*
 * Write the complete buffer to the socket, irrespective of short
 * writes or interrupts.
 */
static int
write_socket(int s, const void *buf, size_t size)
{
	const char *data = (const char *) buf;
	size_t total_count = 0;

	while (total_count < size) {
		ssize_t count
			= write(s, data + total_count, size - total_count);
		if (count == -1) {
			if (errno != EAGAIN) {
				error("network write failed: %s",
				      strerror(errno));
				return 0;
			} else {
				continue;
			}
		}
		total_count += count;
	}

	return 1;
}

/*
 * Read SIZE bytes from the socket into BUF.  Keep reading unless an
 * error occurs (except for EAGAIN) or EOF is reached.
 */
static int
read_socket(int s, void *buf, size_t size)
{
	char *data = (char *) buf;
	size_t total_count = 0;

	while (total_count < size) {
		ssize_t count = read(s, data + total_count, size - total_count);
		if (count == -1) {
			/* Error or interrupt.  */
			if (errno != EAGAIN) {
				error("network read failed: %s",
				      strerror(errno));
				return 0;
			} else {
				continue;
			}
		} else if (count == 0) {
			/* End of file (connection closed?)  */
			error("network read failed: Connection closed by peer");
			return 0;
		}
		total_count += count;
	}

	return 1;
}

static int
print_rdata(buffer_type *output, rrtype_descriptor_type *descriptor,
	    rr_type *record)
{
	size_t i;
	size_t saved_position = buffer_position(output);
	
	for (i = 0; i < record->rdata_count; ++i) {
		if (i == 0) {
			buffer_printf(output, "\t");
		} else if (descriptor->type == TYPE_SOA && i == 2) {
			buffer_printf(output, " (\n\t\t");
		} else {
			buffer_printf(output, " ");
		}
		if (!rdata_atom_to_string(
			    output,
			    (rdata_kind_type) descriptor->rdata_kinds[i],
			    record->rdatas[i]))
		{
			buffer_set_position(output, saved_position);
			return 0;
		}
	}
	if (descriptor->type == TYPE_SOA) {
		buffer_printf(output, " )");
	}
	
	return 1;
}

static void
set_previous_owner(axfr_state_type *state, const dname_type *dname)
{
	region_free_all(state->previous_owner_region);
	state->previous_owner = dname_copy(state->previous_owner_region, dname);
	state->previous_owner_origin = dname_origin(
		state->previous_owner_region, state->previous_owner);
}

static int
print_rr(FILE *out,
	 axfr_state_type *state,
	 rr_type *record)
{
	buffer_type *output = buffer_create(state->rr_region, 1000);
	rrtype_descriptor_type *descriptor
		= rrtype_descriptor_by_type(record->type);
	int result;
	const dname_type *owner = domain_dname(record->owner);
	const dname_type *owner_origin
		= dname_origin(state->rr_region, owner);
	int owner_changed
		= (!state->previous_owner
		   || dname_compare(state->previous_owner, owner) != 0);
	if (owner_changed) {
		int origin_changed = (!state->previous_owner_origin
				      || dname_compare(
					      state->previous_owner_origin,
					      owner_origin) != 0);
		if (origin_changed) {
			buffer_printf(
				output,
				"$ORIGIN %s\n",
				dname_to_string(owner_origin, NULL));
		}
	
		set_previous_owner(state, owner);
		buffer_printf(output,
			      "%s",
			      dname_to_string(owner,
					      state->previous_owner_origin));
	}
	
	buffer_printf(output,
		      "\t%lu\t%s\t%s",
		      (unsigned long) record->ttl,
		      rrclass_to_string(record->klass),
		      rrtype_to_string(record->type));

	result = print_rdata(output, descriptor, record);
	if (!result) {
		/*
		 * Some RDATA failed to print, so print the record's
		 * RDATA in unknown format.
		 */
		result = rdata_atoms_to_unknown_string(output,
						       descriptor,
						       record->rdata_count,
						       record->rdatas);
	}
	
	if (result) {
		buffer_printf(output, "\n");
		buffer_flip(output);
		fwrite(buffer_current(output), buffer_remaining(output), 1,
		       out);
/* 		fflush(out); */
	}
	
	return result;
}


static int
parse_response(FILE *out, axfr_state_type *state)
{
	size_t rr_count;
	size_t qdcount = QDCOUNT(state->q->packet);
	size_t ancount = ANCOUNT(state->q->packet);

	/* Skip question section.  */
	for (rr_count = 0; rr_count < qdcount; ++rr_count) {
		if (!packet_skip_rr(state->q->packet, 1)) {
			error("bad RR in question section");
			return 0;
		}
	}

	/* Read RRs from answer section and print them.  */
	for (rr_count = 0; rr_count < ancount; ++rr_count) {
		domain_table_type *owners
			= domain_table_create(state->rr_region);
		rr_type *record = packet_read_rr(
			state->rr_region, owners, state->q->packet, 0);
		if (!record) {
			error("bad RR in answer section");
			return 0;
		}

		if (state->rr_count == 0
		    && (record->type != TYPE_SOA || record->klass != CLASS_IN))
		{
			error("First RR must be the SOA record, but is a %s record",
			      rrtype_to_string(record->type));
			return 0;
		} else if (state->rr_count > 0
			   && record->type == TYPE_SOA
			   && record->klass == CLASS_IN)
		{
			state->done = 1;
			return 1;
		}
		
		++state->rr_count;
		
		if (!print_rr(out, state, record)) {
			return 0;
		}
		
		region_free_all(state->rr_region);
	}

	return 1;
}

static int
send_query(int s, query_type *q)
{
	uint16_t size = htons(buffer_remaining(q->packet));

	if (!write_socket(s, &size, sizeof(size))) {
		return 0;
	}
	if (!write_socket(s, buffer_begin(q->packet), buffer_limit(q->packet)))
	{
		return 0;
	}
	return 1;
}

static int
receive_response_no_timeout(axfr_state_type *state)
{
	uint16_t size;
	
	buffer_clear(state->q->packet);
	if (!read_socket(state->s, &size, sizeof(size))) {
		return 0;
	}
	size = ntohs(size);
	if (size > state->q->maxlen) {
		error("response size (%d) exceeds maximum (%d)",
		      (int) size, (int) state->q->maxlen);
		return 0;
	}
	if (!read_socket(state->s, buffer_begin(state->q->packet), size)) {
		return 0;
	}

	buffer_set_position(state->q->packet, size);

	++state->packets_received;
	state->bytes_received += sizeof(size) + size;

	return 1;
}

static int
receive_response(axfr_state_type *state)
{
	int result;
	
	timeout_flag = 0;
	alarm(MAX_WAITING_TIME);
	result = receive_response_no_timeout(state);
	alarm(0);
	if (!result && timeout_flag) {
		error("timeout reading response, server unreachable?");
	}
	
	return result;
}

static int
check_response_tsig(query_type *q, tsig_record_type *tsig)
{
	if (!tsig)
		return 1;

	if (!tsig_find_rr(tsig, q->packet)) {
		error("error parsing response");
		return 0;
	}
	if (tsig->status == TSIG_NOT_PRESENT) {
		if (tsig->response_count == 0) {
			error("required TSIG not present");
			return 0;
		}
		if (tsig->updates_since_last_prepare > 100) {
			error("too many response packets without TSIG");
			return 0;
		}
		tsig_update(tsig, q->packet, buffer_limit(q->packet));
		return 1;
	}
	
	ARCOUNT_SET(q->packet, ARCOUNT(q->packet) - 1);
	
	if (tsig->status == TSIG_ERROR) {
		error("TSIG record is not correct");
		return 0;
	} else if (tsig->error_code != TSIG_ERROR_NOERROR) {
		error("TSIG error code: %s",
		      tsig_error(tsig->error_code));
		return 0;
	} else {
		tsig_update(tsig, q->packet, tsig->position);
		if (!tsig_verify(tsig)) {
			error("TSIG record did not authenticate");
			return 0;
		}
		tsig_prepare(tsig);
	}

	return 1;
}


/*
 * Query the server for the zone serial. Return 1 if the zone serial
 * is higher than the current serial, 0 if the zone serial is lower or
 * equal to the current serial, and -1 on error.
 *
 * On success, the zone serial is returned in ZONE_SERIAL.
 */
static int
check_serial(axfr_state_type *state)
{
	region_type *local;
	uint16_t query_id;
	uint16_t i;
	domain_table_type *owners;
	
	query_id = init_query(
		state->q, state->zone, TYPE_SOA, CLASS_IN, state->tsig);
	
	if (!send_query(state->s, state->q)) {
		return -1;
	}

	if (state->tsig) {
		/* Prepare for checking responses. */
		tsig_prepare(state->tsig);
	}
	
	if (!receive_response(state)) {
		return -1;
	}
	buffer_flip(state->q->packet);

	if (buffer_limit(state->q->packet) <= QHEADERSZ) {
		error("response size (%d) is too small",
		      (int) buffer_limit(state->q->packet));
		return -1;
	}

	if (!QR(state->q->packet)) {
		error("response is not a response");
		return -1;
	}

	if (TC(state->q->packet)) {
		error("response is truncated");
		return -1;
	}
	
	if (ID(state->q->packet) != query_id) {
		error("bad response id (%d), expected (%d)",
		      (int) ID(state->q->packet), (int) query_id);
		return -1;
	}
	
	if (RCODE(state->q->packet) != RCODE_OK) {
		error("error response %d", (int) RCODE(state->q->packet));
		return -1;
	}
	
	if (QDCOUNT(state->q->packet) != 1) {
		error("question section count not equal to 1");
		return -1;
	}
	
	if (ANCOUNT(state->q->packet) == 0) {
		error("answer section is empty");
		return -1;
	}

	if (!check_response_tsig(state->q, state->tsig)) {
		return -1;
	}
	
	buffer_set_position(state->q->packet, QHEADERSZ);

	local = region_create(xalloc, free);
	owners = domain_table_create(local);
	
	/* Skip question records. */
	for (i = 0; i < QDCOUNT(state->q->packet); ++i) {
		rr_type *record
			= packet_read_rr(local, owners, state->q->packet, 1);
		if (!record) {
			error("bad RR in question section");
			region_destroy(local);
			return -1;
		}

		if (dname_compare(state->zone, domain_dname(record->owner)) != 0
		    || record->type != TYPE_SOA
		    || record->klass != CLASS_IN)
		{
			error("response does not match query");
			region_destroy(local);
			return -1;
		}
	}
	
	/* Find the SOA record in the response.  */
	for (i = 0; i < ANCOUNT(state->q->packet); ++i) {
		rr_type *record
			= packet_read_rr(local, owners, state->q->packet, 0);
		if (!record) {
			error("bad RR in answer section");
			region_destroy(local);
			return -1;
		}

		if (dname_compare(state->zone, domain_dname(record->owner)) == 0
		    && record->type == TYPE_SOA
		    && record->klass == CLASS_IN)
		{
			assert(record->rdata_count == 7);
			assert(rdata_atom_size(record->rdatas[2]) == 4);
			state->zone_serial = read_uint32(
				rdata_atom_data(record->rdatas[2]));
			region_destroy(local);
			return (state->first_transfer
				|| state->zone_serial > state->last_serial);
		}
	}

	error("SOA not found in answer");
	region_destroy(local);
	return -1;
}

/*
 * Receive and parse the AXFR response packets.
 */
static int
handle_axfr_response(FILE *out, axfr_state_type *axfr)
{
	while (!axfr->done) {
		if (!receive_response(axfr)) {
			return 0;
		}
		
		buffer_flip(axfr->q->packet);
		
		if (buffer_limit(axfr->q->packet) <= QHEADERSZ) {
			error("response size (%d) is too small",
			      (int) buffer_limit(axfr->q->packet));
			return 0;
		}

		if (!QR(axfr->q->packet)) {
			error("response is not a response");
			return 0;
		}

		if (ID(axfr->q->packet) != axfr->query_id) {
			error("bad response id (%d), expected (%d)",
			      (int) ID(axfr->q->packet),
			      (int) axfr->query_id);
			return 0;
		}

		if (RCODE(axfr->q->packet) != RCODE_OK) {
			error("error response %d",
			      (int) RCODE(axfr->q->packet));
			return 0;
		}

		if (QDCOUNT(axfr->q->packet) > 1) {
			error("query section count greater than 1");
			return 0;
		}

		if (ANCOUNT(axfr->q->packet) == 0) {
			error("answer section is empty");
			return 0;
		}

		if (!check_response_tsig(axfr->q, axfr->tsig)) {
			return 0;
		}
	
		buffer_set_position(axfr->q->packet, QHEADERSZ);
		
		if (!parse_response(out, axfr)) {
			return 0;
		}
	}
	return 1;
}

static int
axfr(FILE *out, axfr_state_type *state, const char *server)
{
	state->query_id = init_query(
		state->q, state->zone, TYPE_AXFR, CLASS_IN, state->tsig);

	log_msg(LOG_INFO,
		"send AXFR query to %s for %s",
		server,
		dname_to_string(state->zone, NULL));
	
	if (!send_query(state->s, state->q)) {
		return 0;
	}

	if (state->tsig) {
		/* Prepare for checking responses.  */
		tsig_prepare(state->tsig);
	}
	
	return handle_axfr_response(out, state);
}

static uint16_t
init_query(query_type *q,
	   const dname_type *dname,
	   uint16_t type,
	   uint16_t klass,
	   tsig_record_type *tsig)
{
	uint16_t query_id = (uint16_t) random();
	
	buffer_clear(q->packet);
	
	/* Set up the header */
	ID_SET(q->packet, query_id);
	FLAGS_SET(q->packet, 0);
	OPCODE_SET(q->packet, OPCODE_QUERY);
	AA_SET(q->packet);
	QDCOUNT_SET(q->packet, 1);
	ANCOUNT_SET(q->packet, 0);
	NSCOUNT_SET(q->packet, 0);
	ARCOUNT_SET(q->packet, 0);
	buffer_skip(q->packet, QHEADERSZ);

	/* The question record.  */
	buffer_write(q->packet, dname_name(dname), dname_length(dname));
	buffer_write_u16(q->packet, type);
	buffer_write_u16(q->packet, klass);

	if (tsig) {
		tsig_init_query(tsig, query_id);
		tsig_prepare(tsig);
		tsig_update(tsig, q->packet, buffer_position(q->packet));
		tsig_sign(tsig);
		tsig_append_rr(tsig, q->packet);
		ARCOUNT_SET(q->packet, 1);
	}
	
	buffer_flip(q->packet);

	return ID(q->packet);
}

static void
print_zone_header(FILE *out, axfr_state_type *state, const char *server)
{
	time_t now = time(NULL);
	fprintf(out, "; NSD version %s\n", PACKAGE_VERSION);
	fprintf(out, "; zone '%s'", dname_to_string(state->zone, NULL));
	if (state->first_transfer) {
		fprintf(out, "   first transfer\n");
	} else {
		fprintf(out,
			"   last serial %lu\n",
			(unsigned long) state->last_serial);
	}
	fprintf(out, "; from %s using AXFR at %s", server, ctime(&now));
	if (state->tsig) {
		fprintf(out, "; TSIG verified with key '%s'\n",
			dname_to_string(state->tsig->key->name, NULL));
	} else {
		fprintf(out, "; NOT TSIG verified\n");
	}
}

static void
print_stats(axfr_state_type *state)
{
	log_msg(LOG_INFO,
		"received %lu RRs in %lu bytes (using %lu response packets)",
		(unsigned long) state->rr_count,
		(unsigned long) state->bytes_received,
		(unsigned long) state->packets_received);
}

static void
cleanup_region(void *data)
{
	region_type *region = (region_type *) data;
	region_destroy(region);
}

int 
main(int argc, char *argv[])
{
	region_type *region = region_create(xalloc, free);
	int c;
	query_type q;
	struct addrinfo hints, *res0, *res;
	const char *zone_filename = NULL;
	const char *port = TCP_PORT;
	int default_family = DEFAULT_AI_FAMILY;
	struct sigaction mysigaction;
	FILE *zone_file;
	const char *tsig_key_filename = NULL;
	tsig_key_type *tsig_key = NULL;
	axfr_state_type state;
	
	log_init("nsd-xfer");

	/* Initialize the query.  */
	memset(&q, 0, sizeof(query_type));
	q.region = region;
	q.addrlen = sizeof(q.addr);
	q.packet = buffer_create(region, QIOBUFSZ);
	q.maxlen = TCP_MAX_MESSAGE_LEN;

	/* Initialize the state.  */
	state.verbose = 0;
	state.packets_received = 0;
	state.bytes_received = 0;
	state.q = &q;
	state.tsig = NULL;
	state.zone = NULL;
	state.first_transfer = 1;
	state.done = 0;
	state.rr_count = 0;
	state.rr_region = region_create(xalloc, free);
	state.previous_owner_region = region_create(xalloc, free);
	state.previous_owner = NULL;
	state.previous_owner_origin = NULL;

	region_add_cleanup(region, cleanup_region, state.previous_owner_region);
	region_add_cleanup(region, cleanup_region, state.rr_region);
	
	srandom((unsigned long) getpid() * (unsigned long) time(NULL));

	if (!tsig_init(region)) {
		error("TSIG initialization failed");
	}

	/* Parse the command line... */
	while ((c = getopt(argc, argv, "46f:hp:s:T:vz:")) != -1) {
		switch (c) {
		case '4':
			default_family = AF_INET;
			break;
		case '6':
#ifdef INET6
			default_family = AF_INET6;
#else /* !INET6 */
			error("IPv6 support not enabled.");
#endif /* !INET6 */
			break;
		case 'f':
			zone_filename = optarg;
			break;
		case 'h':
			usage();
			break;
		case 'p':
			port = optarg;
			break;
		case 's': {
			const char *t;
			state.first_transfer = 0;
			state.last_serial = strtottl(optarg, &t);
			if (*t != '\0') {
				error("bad serial '%s'", optarg);
				exit(XFER_FAIL);
			}
			break;
		}
		case 'T':
			tsig_key_filename = optarg;
			break;
		case 'v':
			++state.verbose;
			break;
		case 'z':
			state.zone = dname_parse(region, optarg);
			if (!state.zone) {
				error("incorrect domain name '%s'", optarg);
			}
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0 || !zone_filename || !state.zone)
		usage();

	if (tsig_key_filename) {
		tsig_algorithm_type *md5
			= tsig_get_algorithm_by_name("hmac-md5");
		if (!md5) {
			error("cannot initialize hmac-md5: TSIG support not"
			      " enabled");
			exit(XFER_FAIL);
		}
		
		tsig_key = read_tsig_key(
			region, tsig_key_filename, default_family);
		if (!tsig_key) {
			exit(XFER_FAIL);
		}

		tsig_add_key(tsig_key);
		
		state.tsig = (tsig_record_type *) region_alloc(
			region, sizeof(tsig_record_type));
		tsig_init_record(state.tsig, region, md5, tsig_key);
	}

	mysigaction.sa_handler = to_alarm;
	sigfillset(&mysigaction.sa_mask);
	mysigaction.sa_flags = 0;
	if (sigaction(SIGALRM, &mysigaction, NULL) < 0) {
		error("cannot set signal handler");
	}
	
	for (; *argv; ++argv) {
		/* Try each server separately until one succeeds.  */
		int rc;
		
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = default_family;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		rc = getaddrinfo(*argv, port, &hints, &res0);
		if (rc) {
			warning("skipping bad address %s: %s\n",
				*argv,
				gai_strerror(rc));
			continue;
		}

		for (res = res0; res; res = res->ai_next) {
			if (res->ai_addrlen > sizeof(q.addr))
				continue;

			state.s = socket(res->ai_family, res->ai_socktype,
					 res->ai_protocol);
			if (state.s == -1)
				continue;

			if (connect(state.s, res->ai_addr, res->ai_addrlen) < 0)
			{
				warning("cannot connect to %s: %s\n",
					*argv,
					strerror(errno));
				close(state.s);
				continue;
			}
			
			memcpy(&q.addr, res->ai_addr, res->ai_addrlen);

			rc = check_serial(&state);
			if (rc == -1) {
				close(state.s);
				continue;
			}
			if (rc == 0) {
				/* Zone is up-to-date.  */
				close(state.s);
				exit(XFER_UPTODATE);
			} else if (rc > 0) {
				zone_file = fopen(zone_filename, "w");
				if (!zone_file) {
					error("cannot open or create zone file '%s' for writing: %s",
					      zone_filename, strerror(errno));
					close(state.s);
					exit(XFER_FAIL);
				}

				print_zone_header(zone_file, &state, *argv);
				
				if (axfr(zone_file, &state, *argv)) {
					/* AXFR succeeded, done.  */
					fclose(zone_file);
					close(state.s);

					if (state.verbose > 0) {
						print_stats(&state);
					}
					
					exit(XFER_SUCCESS);
				}

				fclose(zone_file);
			}
			
			close(state.s);
		}

		freeaddrinfo(res0);
	}

	log_msg(LOG_ERR,
		"cannot contact an authoritative server, zone NOT transferred");
	exit(XFER_FAIL);
}
