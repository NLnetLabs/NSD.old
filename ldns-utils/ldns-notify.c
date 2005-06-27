/* 
 * nsd-notify.c - nsd-notify(8)
 * 
 * Copyright (c) 2001-2005, NLnet Labs, All right reserved
 *
 * See LICENSE for the license
 *
 * send a notify packet to a server
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>


#include <nsd.h>
#include <options.h>

#include <ldns/dns.h>

static void
usage(void)
{
	fprintf(stderr, "usage: nsd-notify [-h] [-c config-file] -z zone\n");
	fprintf(stderr, "NSD notify utility\n\n");
	fprintf(stderr, " Supported options:\n");
	fprintf(stderr, "\t-c config-file\tSpecify the configuration file\n");
	fprintf(stderr, "\t-z zone\t The zone\n");
	fprintf(stderr, "\t-v\t\tPrint version information\n");
	fprintf(stderr, "\t-h\t\tPrint this help information\n\n");
	fprintf(stderr, "Report bugs to <nsd-bugs@nlnetlabs.nl>\n");
	exit(EXIT_FAILURE);
}

static void
version(void)
{
        fprintf(stderr, "%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
        fprintf(stderr, "Written by NLnet Labs.\n\n");
        fprintf(stderr,
                "Copyright (C) 2001-2005 NLnet Labs.  This is free software.\n"
                "There is NO warranty; not even for MERCHANTABILITY or FITNESS\n"
                "FOR A PARTICULAR PURPOSE.\n");
        exit(EXIT_SUCCESS);
}

int
main(int argc, char **argv)
{
	ldns_pkt *notify;
	ldns_rr *question;
	ldns_rdf *helper;
	ldns_resolver *res;
	
	notify = ldns_pkt_new();
	question = ldns_rr_new();
	res = ldns_resolver_new();

	if (!notify || !question || !res) {
		/* bail out */
		return EXIT_FAILURE;
	}
	/* get the port and nameserver ip from the config */
	ldns_resolver_set_port(res, LDNS_PORT);
	/* ldns_resolver_push_nameserver(res, ns); */

	/* create the rr */
	ldns_rr_set_class(question, LDNS_RR_CLASS_IN);

	helper = ldns_dname_new_frm_str("miek.nl");
	ldns_rr_set_owner(question, helper);

	ldns_rr_set_type(question, LDNS_RR_TYPE_SOA);

	ldns_pkt_set_opcode(notify, LDNS_PACKET_NOTIFY);
	ldns_pkt_push_rr(notify, LDNS_PACKET_QUESTION, question);
	ldns_pkt_set_aa(notify, true);
	ldns_pkt_set_id(notify, 42); /* from nsd-notify... */

	ldns_pkt_print(stdout, notify);

	/*ldns_resolver_send_pkt(NULL, res, notify)*/
        return EXIT_SUCCESS;
}
