#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_net.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_hash.h>
#include <rte_errno.h>
#include <rte_string_fns.h>


static volatile bool force_quit;

/* Debug mode */
static int debug = 0;

#ifndef RTE_MAX_LCORE
#define RTE_MAX_LCORE 4
#endif

#define RTE_LOGTYPE_DPDKLATENCY RTE_LOGTYPE_USER1

#define NB_MBUF   8192

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256
#define NB_SOCKETS 8

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
#define MAX_RX_QUEUE_PER_LCORE 16
#define RSS_HASH_KEY_LENGTH 40

#define DNS_QR_QUERY 0
#define DNS_QR_RESPONSE 1
#define DNS_MAX_NAME_LEN 64
#define DNS_MAX_A_RECORDS 8

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* mask of enabled ports */
static uint32_t dpdklatency_enabled_port_mask = 0;

struct mbuf_table {
	unsigned len;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct dns_info {
    uint8_t qr_type;
    uint16_t msg_id;
    char query_name[DNS_MAX_NAME_LEN];
    uint32_t a_record[DNS_MAX_A_RECORDS];
};

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

/* Magic hash key for symmetric RSS */
static uint8_t hash_key[RSS_HASH_KEY_LENGTH] = { 0x6D, 0x5A, 0x6D, 0x5A, 0x6D,
	0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D,
	0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, };

/* Port configuration structure */
static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode    = ETH_MQ_RX_RSS, /**< RSS enables */
		.split_hdr_size = 0,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = hash_key,
			.rss_hf = ETH_RSS_PROTO_MASK,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * dpdklatency_pktmbuf_pool = NULL;

/* Per-lcore (essentially queue) statistics struct */
struct dpdklatency_lcore_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct dpdklatency_lcore_statistics lcore_statistics[RTE_MAX_LCORE];

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

#define TIMESTAMP_HASH_ENTRIES 99999

typedef struct rte_hash lookup_struct_t;
static lookup_struct_t *ipv4_tcp_timestamp_lookup_struct[NB_SOCKETS];


#ifdef RTE_MACHINE_CPUFLAG_SSE4_2
#include <rte_hash_crc.h>
#define DEFAULT_HASH_FUNC       rte_hash_crc
#else
#include <rte_jhash.h>
#define DEFAULT_HASH_FUNC       rte_jhash
#endif

#define CLOCK_PRECISION 1000000000L /* one billion */

struct lcore_rx_queue {
	uint8_t port_id;
	uint8_t queue_id;
} __rte_cache_aligned;

#define MAX_LCORE_PARAMS 1024
struct lcore_params {
	uint8_t port_id;
	uint8_t queue_id;
	uint8_t lcore_id;
} __rte_cache_aligned;

// Configure port-queue-lcore assigment here
static struct lcore_params lcore_params_array[MAX_LCORE_PARAMS];
static struct lcore_params lcore_params_array_default[] = {
	{0, 0, 1},
	{0, 1, 2},
	{0, 2, 3},
	{0, 3, 4},
};

static struct lcore_params * lcore_params = lcore_params_array_default;
static uint16_t nb_lcore_params = sizeof(lcore_params_array_default) /
				sizeof(lcore_params_array_default[0]);

struct lcore_conf {
	uint16_t n_rx_queue;
	struct lcore_rx_queue rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
	struct mbuf_table tx_mbufs[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

static struct lcore_conf lcore_conf[RTE_MAX_LCORE] __rte_cache_aligned;

FILE * output_file = NULL;

inline static void data_output(char * message) {
    if (output_file != NULL) {
        fputs(message, output_file);
    }
}

inline static uint64_t monotonic_time_nanosecs()
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)(CLOCK_PRECISION * timestamp.tv_sec) + timestamp.tv_nsec;
}

inline static uint64_t timestamp_millisecs()
{
    struct timespec timestamp;
    clock_gettime(CLOCK_REALTIME, &timestamp);
    return (uint64_t)(1000 * timestamp.tv_sec) + (timestamp.tv_nsec / 1000000);
}

static void
send_rtt_tcp_ipv4(uint32_t sourceip, uint32_t destip, unsigned long rtt_usecs, uint64_t timestamp_msecs)
{
    struct in_addr inaddr;
	char message[6+1+13+1+15+1+15+1+10+2];
	char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];

	inaddr.s_addr = sourceip;
	inet_ntop(AF_INET, &inaddr, src_ip, sizeof(src_ip));
	inaddr.s_addr = destip;
	inet_ntop(AF_INET, &inaddr, dst_ip, sizeof(dst_ip));

	snprintf(message, sizeof(message), "RTTTCP\t%13llu\t%s\t%s\t%10llu\n",
		timestamp_msecs, src_ip, dst_ip, rtt_usecs);

	if (unlikely(debug)){
		printf("%s", message);
		fflush(stdout);
	}

	data_output(message);
}


static void
track_latency_syn_v4(uint64_t key, uint64_t *ipv4_timestamp_syn)
{
	int ret = 0;
	unsigned lcore_id;

	lcore_id = rte_lcore_id();

	ret = rte_hash_add_key (ipv4_tcp_timestamp_lookup_struct[lcore_id], (void *) &key);
	if (unlikely(debug)) {
		printf("SYN lcore %u, ret: %d \n", lcore_id, ret);
	}
	if (ret < 0) {
		RTE_LOG(INFO, DPDKLATENCY, "Hash table full for lcore %u - clearing it\n", lcore_id);
		rte_hash_reset(ipv4_tcp_timestamp_lookup_struct[lcore_id]);
		ret = rte_hash_add_key(ipv4_tcp_timestamp_lookup_struct[lcore_id], (void *) &key);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "Unable to add SYN timestamp to hash after cleaning it");
		}
	}
	ipv4_timestamp_syn[ret] = (uint64_t) monotonic_time_nanosecs();
}

static void
track_latency_ack_v4(uint64_t key, uint32_t sourceip, uint32_t destip, const uint64_t *ipv4_timestamp_syn)
{
	unsigned lcore_id;
    unsigned long long elapsed;
	int ret = 0;

	lcore_id = rte_lcore_id();
	// printf("start processing tcp ack on lcore %d\n", lcore_id);

	ret = rte_hash_lookup(ipv4_tcp_timestamp_lookup_struct[lcore_id], (const void *) &key);
	// printf("hash lookup: %d\n", ret);
	if (ret >= 0) {
        elapsed = monotonic_time_nanosecs() - ipv4_timestamp_syn[ret];
		printf("SYN-ACK %d %lu microsecs from %08x to %08x\n", ret, (unsigned long) (elapsed / 1000), sourceip, destip);
		// limit elapsed time
		if ((elapsed / 1000) < 10000000000L) {
            send_rtt_tcp_ipv4(destip, sourceip, (unsigned long) (elapsed / 1000), timestamp_millisecs());
		}
		rte_hash_del_key (ipv4_tcp_timestamp_lookup_struct[lcore_id], (void *) &key);
	}
}

static int
parse_dns(u_char * buffer, uint16_t diagram_len, struct dns_info * dnsInfo) {
    ns_msg nsMsg;
    if (ns_initparse(buffer, diagram_len - 8, &nsMsg) != 0) {
        return -1;
    }
    if (ns_msg_getflag(nsMsg, ns_f_rcode) != ns_r_noerror) {
        return -2;
    }
    if (ns_msg_getflag(nsMsg, ns_f_opcode) != ns_o_query) {
        return 1;
    }
    if (ns_msg_count(nsMsg, ns_s_qd) != 1) {
        return 2;
    }

    int ans_count = ns_msg_count(nsMsg, ns_s_an);

    uint16_t msg_id = (uint16_t) ns_msg_id(nsMsg);
    int flag_qr = ns_msg_getflag(nsMsg, ns_f_qr);

    if (flag_qr == DNS_QR_QUERY) { // query
        dnsInfo->qr_type = DNS_QR_QUERY;
        dnsInfo->msg_id = msg_id;

    } else { // response
        ns_rr rr;
        if (ns_parserr(&nsMsg, ns_s_qd, 0, &rr) != 0) {
            return -3;
        }
        char * name = ns_rr_name(rr);
        size_t name_len = strlen(name);
        if (name_len < DNS_MAX_NAME_LEN) {
            strcpy(dnsInfo->query_name, name);
        } else {
            strncpy(dnsInfo->query_name, name, DNS_MAX_NAME_LEN - 1);
            dnsInfo->query_name[DNS_MAX_NAME_LEN - 1] = '\0';
        }

        int a_count = 0, i = 0;
        for (; i < ans_count && a_count < DNS_MAX_A_RECORDS; i++) {
            if (ns_parserr(&nsMsg, ns_s_an, i, &rr) != 0) {
                continue;
            }
            if (ns_rr_type(rr) == ns_t_a && ns_rr_class(rr) == ns_c_in) {
                dnsInfo->a_record[a_count] = (uint32_t) ns_get32(ns_rr_rdata(rr));
                a_count++;
            }
        }

        for (; a_count < DNS_MAX_A_RECORDS; a_count++) {
            dnsInfo->a_record[a_count] = 0;
        }

        dnsInfo->msg_id = msg_id;
        dnsInfo->qr_type = DNS_QR_RESPONSE;
    }

    return 0;
}

static void
track_latency(struct rte_mbuf *m, uint64_t *ipv4_timestamp_syn)
{
	struct ether_hdr *eth_hdr;
	struct tcp_hdr *tcp_hdr = NULL;
	struct udp_hdr *udp_hdr = NULL;
	struct ipv4_hdr* ipv4_hdr;
	enum { URG_FLAG = 0x20, ACK_FLAG = 0x10, PSH_FLAG = 0x08, RST_FLAG = 0x04, SYN_FLAG = 0x02, FIN_FLAG = 0x01 };
	uint16_t tcp_seg_len;
	uint64_t key;
//	unsigned lcore_id = rte_lcore_id();

	eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);

	/* ignore non-ipv4 packets (including vlan) */
	if (eth_hdr->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
		return;
	}
	
	// IPv4	
	ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *, sizeof(struct ether_hdr));
	if (ipv4_hdr->next_proto_id == IPPROTO_TCP) {
		tcp_hdr = rte_pktmbuf_mtod_offset(m, struct tcp_hdr *,
			sizeof(struct ipv4_hdr) + sizeof(struct ether_hdr));
		// printf("tcp_flags: %u\n", tcp_hdr->tcp_flags);
		tcp_seg_len = (uint16_t) (rte_be_to_cpu_16(ipv4_hdr->total_length) - ((ipv4_hdr->version_ihl & 0x0f) << 2) - (tcp_hdr->data_off >> 4 << 2));
		// printf("seglen %u = %u - %u - %u\n", tcp_seg_len, rte_be_to_cpu_16(ipv4_hdr->total_length), ((ipv4_hdr->version_ihl & 0x0f) << 2), (tcp_hdr->data_off >> 4 << 2));
		// printf("SYNACK lcore %u hash %x src %x dst %x seq %u ack %u len %u\n", lcore_id, m->hash.rss, rte_be_to_cpu_32(ipv4_hdr->src_addr), rte_be_to_cpu_32(ipv4_hdr->dst_addr), rte_be_to_cpu_32(tcp_hdr->sent_seq), rte_be_to_cpu_32(tcp_hdr->recv_ack), tcp_seg_len);
		
		switch (tcp_hdr->tcp_flags){
			case SYN_FLAG | ACK_FLAG:
				key = (long long) m->hash.rss << 32 | (rte_be_to_cpu_32(tcp_hdr->sent_seq) + 1);
				track_latency_syn_v4(key, ipv4_timestamp_syn);
				break;
			case ACK_FLAG | PSH_FLAG:
				key = (long long) m->hash.rss << 32 | (rte_be_to_cpu_32(tcp_hdr->sent_seq) + tcp_seg_len);
				track_latency_syn_v4(key, ipv4_timestamp_syn);
				break;
			case ACK_FLAG:
				key = (long long) m->hash.rss << 32 | (rte_be_to_cpu_32(tcp_hdr->recv_ack));
				track_latency_ack_v4(key,
                                     ipv4_hdr->dst_addr,
                                     ipv4_hdr->src_addr,
                                     ipv4_timestamp_syn);
				break;
            default:
                break;
		}

	} else if (ipv4_hdr->next_proto_id == IPPROTO_UDP) {

	    udp_hdr = rte_pktmbuf_mtod_offset(m, struct udp_hdr *, sizeof(struct ipv4_hdr) + sizeof(struct ether_hdr));
	    /* recognize dns packet */
	    if (udp_hdr->src_port == 53 || udp_hdr->dst_port == 53) {
	        struct dns_info dnsInfo;
            int ret = parse_dns(
                    rte_pktmbuf_mtod_offset(m, u_char *, sizeof(struct ipv4_hdr) + sizeof(struct ether_hdr) + sizeof(struct udp_hdr)),
                    udp_hdr->dgram_len, &dnsInfo);
            if (ret == 0) {
                // todo
                printf("msgid %x type %d name %s\n%x %x %x %x\n%x %x %x %x\n", dnsInfo.msg_id, dnsInfo.qr_type, dnsInfo.query_name,
                       dnsInfo.a_record[0], dnsInfo.a_record[1], dnsInfo.a_record[2], dnsInfo.a_record[3],
                       dnsInfo.a_record[4], dnsInfo.a_record[5], dnsInfo.a_record[6], dnsInfo.a_record[7]);

            } else {
                printf("dns parse error %d\n", ret);
            }
	    }
	}
}

/* Send the burst of packets on an output interface */
static int
dpdklatency_send_burst(struct lcore_conf *qconf, unsigned n, uint8_t port)
{
	struct rte_mbuf **m_table;
	unsigned ret;
	unsigned queueid = 0;
	unsigned lcore_id = rte_lcore_id();

	m_table = (struct rte_mbuf **)qconf->tx_mbufs[port].m_table;

	// TODO: change here is more than one TX queue per port is required
	ret = rte_eth_tx_burst(port, (uint16_t) queueid, m_table, (uint16_t) n);
	lcore_statistics[lcore_id].tx += ret;
	if (unlikely(ret < n)) {
		lcore_statistics[lcore_id].dropped += (n - ret);
		do {
			rte_pktmbuf_free(m_table[ret]);
		} while (++ret < n);
	}

	return 0;
}

/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned lcore_id;

	// TODO: dopped TX packets are not counted properly
	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nLcore statistics ====================================");

	RTE_LCORE_FOREACH(lcore_id) {
		printf("\nStatistics for lcore %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   lcore_id,
			   lcore_statistics[lcore_id].tx,
			   lcore_statistics[lcore_id].rx,
			   lcore_statistics[lcore_id].dropped);

		total_packets_dropped += lcore_statistics[lcore_id].dropped;
		total_packets_tx += lcore_statistics[lcore_id].tx;
		total_packets_rx += lcore_statistics[lcore_id].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");
}

/* packet processing loop */
static void
dpdklatency_processing_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i, j, portid, queueid, nb_rx;
	struct lcore_conf *qconf;
	struct rte_mbuf *m;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;
	uint64_t ipv4_timestamp_syn[TIMESTAMP_HASH_ENTRIES] __rte_cache_aligned;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_conf[lcore_id];

	if (qconf->n_rx_queue == 0) {
		RTE_LOG(INFO, DPDKLATENCY, "lcore %u has nothing to do - no RX queue assigned\n", lcore_id);
		return;
	}

	for (i = 0; i < qconf->n_rx_queue; i++) {
		portid = qconf->rx_queue_list[i].port_id;
		queueid = qconf->rx_queue_list[i].queue_id;
		RTE_LOG(INFO, DPDKLATENCY, " -- lcoreid=%u portid=%hhu "
			"rxqueueid=%hhu\n", lcore_id, portid, queueid);
	}

	while (!force_quit) {
		cur_tsc = rte_rdtsc();

		/* TX burst queue drain	 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {
			/* print status by master lcore */
			if (lcore_id == rte_get_master_lcore()) {
				/* if timer is enabled */
				if (timer_period > 0) {
					/* advance the timer */
					timer_tsc += diff_tsc;
					/* if timer has reached its timeout */
					if (unlikely(timer_tsc >= (uint64_t) timer_period)) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
				if (qconf->tx_mbufs[portid].len == 0)
					continue;
				dpdklatency_send_burst(&lcore_conf[lcore_id],
						 qconf->tx_mbufs[portid].len,
						 (uint8_t) portid);
		
				qconf->tx_mbufs[portid].len = 0;
			}
			prev_tsc = cur_tsc;
		}
		
		for (i = 0; i < qconf->n_rx_queue; i++) {
			portid = qconf->rx_queue_list[i].port_id;
			queueid = qconf->rx_queue_list[i].queue_id;

			/* Reading from RX queue */
			nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst, MAX_PKT_BURST);
			if (unlikely(debug && nb_rx > 0)) {
				printf("reading from portid %u, queueid %u, nb_rx %u\n", portid, queueid, nb_rx);
			}
			lcore_statistics[lcore_id].rx += nb_rx;

			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));

				// Call the latency tracker function for every packet
				track_latency(m, ipv4_timestamp_syn);
				// drop it like it's hot
				rte_pktmbuf_free(m);
			}
		}
		usleep(1000);
	}
}

/* display usage */
static void
dpdklatency_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ] [-T PERIOD] [-o FILENAME] [--config (port, queue, lcore)] [--publishto IP] [--debug]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
	       "  -o FILENAME: write output to file\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       " --config (port,queue,lcore)[,(port,queue,lcore)]\n"
	       " --publishto IP: publish to a specific IP (where analytics is running). If not specified, this program binds.\n"
	       " --debug: shows captured flows\n",
	       prgname);
}

static int
dpdklatency_parse_config(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames {
		FLD_PORT = 0,
		FLD_QUEUE,
		FLD_LCORE,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned size;

	nb_lcore_params = 0;

	while ((p = strchr(p0,'(')) != NULL) {
		++p;
		if((p0 = strchr(p,')')) == NULL)
			return -1;

		size = p0 - p;
		if(size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') !=
								_NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++){
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] >
									255)
				return -1;
		}
		if (nb_lcore_params >= MAX_LCORE_PARAMS) {
			printf("exceeded max number of lcore params: %hu\n",
				nb_lcore_params);
			return -1;
		}
		lcore_params_array[nb_lcore_params].port_id =
				(uint8_t)int_fld[FLD_PORT];
		lcore_params_array[nb_lcore_params].queue_id =
				(uint8_t)int_fld[FLD_QUEUE];
		lcore_params_array[nb_lcore_params].lcore_id =
				(uint8_t)int_fld[FLD_LCORE];
		++nb_lcore_params;
	}
	lcore_params = lcore_params_array;

	return 0;
}

static int
dpdklatency_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static long
dpdklatency_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	long n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

/* Parse the argument given in the command line of the application */
static int
dpdklatency_parse_args(int argc, char **argv)
{
	int opt, ret;
    long timer_secs;
    char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{ "config", 1, 0, 0},
		{ "debug", no_argument, &debug, 1},
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:q:T:o:",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			dpdklatency_enabled_port_mask = (uint32_t) dpdklatency_parse_portmask(optarg);
			if (dpdklatency_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				dpdklatency_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_secs = dpdklatency_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				dpdklatency_usage(prgname);
				return -1;
			}
			timer_period = (uint64_t) timer_secs;
			break;

		/* output file */
		case 'o':
		    if (optarg[0] == '-' && optarg[1] == '\0') {
		        output_file = stdout;
		    } else {
                output_file = fopen(optarg, "a");
                if (output_file == NULL) {
                    printf("open file failed: %s\n", optarg);
                    return -1;
                }
            }
			break;

		/* long options */
		case 0:
			if (!strncmp(lgopts[option_index].name, "config", 6)) {
				ret = dpdklatency_parse_config(optarg);
				if (ret) {
					printf("invalid config\n");
					dpdklatency_usage(prgname);
					return -1;
				}
			}
			break;

		default:
			dpdklatency_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint16_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if (force_quit)
				return;
			if ((port_mask & (1u << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

static void
setup_hash(int lcoreid)
{
	struct rte_hash_parameters ipv4_timestamp_hash_params = {
		.name = NULL,
		.entries = TIMESTAMP_HASH_ENTRIES,
		.key_len = sizeof(uint64_t),
		.hash_func = DEFAULT_HASH_FUNC,
		.hash_func_init_val = 0,
	};

	char s[64];

	/* create ipv4 hash */
	snprintf(s, sizeof(s), "ipv4_timestamp_hash_%d", lcoreid);
	ipv4_timestamp_hash_params.name = s;
	ipv4_timestamp_hash_params.socket_id = 0;
	ipv4_tcp_timestamp_lookup_struct[lcoreid] =
		rte_hash_create(&ipv4_timestamp_hash_params);
	if (ipv4_tcp_timestamp_lookup_struct[lcoreid] == NULL)
		rte_exit(EXIT_FAILURE, "Unable to create the timestamp hash on "
				"socket %d\n", 0);
}

static int
init_hash(void)
{
	int socketid = 0;
	unsigned lcore_id;

	RTE_LCORE_FOREACH(lcore_id) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;

		if (socketid >= NB_SOCKETS) {
			rte_exit(EXIT_FAILURE, "Socket %d of lcore %u is "
					"out of range %d\n", socketid,
						lcore_id, NB_SOCKETS);
		}
		printf("Setting up hash table for lcore %u, on socket %u\n", lcore_id, socketid);
		setup_hash(lcore_id);
	}
	return 0;
}


static uint16_t
get_port_n_rx_queues(const uint16_t port)
{
	int queue = -1;
	uint16_t i;

	for (i = 0; i < nb_lcore_params; ++i) {
		if (lcore_params[i].port_id == port &&
				lcore_params[i].queue_id > queue)
			queue = lcore_params[i].queue_id;
	}
	return (uint16_t) ++queue;
}

static int
init_lcore_rx_queues(void)
{
	uint16_t i, nb_rx_queue;
	uint8_t lcore;

	for (i = 0; i < nb_lcore_params; ++i) {
		lcore = lcore_params[i].lcore_id;
		nb_rx_queue = lcore_conf[lcore].n_rx_queue;
		if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
			printf("error: too many queues (%u) for lcore: %u\n",
				(unsigned)nb_rx_queue + 1, (unsigned)lcore);
			return -1;
		} else {	
			lcore_conf[lcore].rx_queue_list[nb_rx_queue].port_id =
				lcore_params[i].port_id;
			lcore_conf[lcore].rx_queue_list[nb_rx_queue].queue_id =
				lcore_params[i].queue_id;
			lcore_conf[lcore].n_rx_queue++;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct lcore_conf *qconf;
	int ret;
	uint16_t nb_ports, nb_ports_available, portid, nb_rx_queue;
	uint8_t queueid, queue;
	char *publish_host;
	unsigned lcore_id;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = dpdklatency_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid DPDKLATENCY arguments\n");

	ret = init_lcore_rx_queues();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "init_lcore_rx_queues failed\n");


	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	/* create the mbuf pool */
	dpdklatency_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (dpdklatency_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	qconf = NULL;
	
	nb_ports_available = nb_ports;

	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((dpdklatency_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", (unsigned) portid);
			nb_ports_available--;
			continue;
		}

		/* init port */
		printf("Initializing port %d ... \n", portid );
		fflush(stdout);

		/* init port */
		nb_rx_queue = get_port_n_rx_queues(portid);
		ret = rte_eth_dev_configure(portid, nb_rx_queue, 1, &port_conf);
		if (ret < 0) 
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, (unsigned) portid);
		
		/* init one TX queue (queue id is 0) on each port */
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				NULL);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, (unsigned) portid);
	

		/* Initialize TX buffers */
		tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					(unsigned) portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

	}

	// TODO: maybe use separate mbuf pool per lcore?	
	/* init hash */
	ret = init_hash();
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "init_hash failed\n");

	/* Init RX queues */
	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;
		qconf = &lcore_conf[lcore_id];	
		for(queue = 0; queue < qconf->n_rx_queue; ++queue) {
			portid = qconf->rx_queue_list[queue].port_id;
			queueid = qconf->rx_queue_list[queue].queue_id;

			printf("setting up rx queue on port %u, queue %u\n", portid, queueid);	
			ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd,
						     rte_eth_dev_socket_id(portid),
					     NULL,
					     dpdklatency_pktmbuf_pool);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					  ret, (unsigned) portid);
		}
	}

	for (portid = 0; portid < nb_ports; portid++) {
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, (unsigned) portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		/* initialize port stats */
		memset(&lcore_statistics, 0, sizeof(lcore_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(nb_ports, dpdklatency_enabled_port_mask);

	ret = 0;

	/* launch processing loop on all core */
	rte_eal_mp_remote_launch((lcore_function_t *) dpdklatency_processing_loop, NULL, CALL_MASTER);

	rte_eal_mp_wait_lcore();

	for (portid = 0; portid < nb_ports; portid++) {
		if ((dpdklatency_enabled_port_mask & (1u << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	printf("Bye...\n");

	return ret;
}
