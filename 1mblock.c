#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>
#include <string.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

#define TRUE 1
#define FALSE 0 
#define HASH_MOD 2470312487

int filter_flag;
char* http_method[6] = {"GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE"};

struct siteinfo {
	int len;
	char firstch;
	unsigned long int urlhash[2];
};

struct siteinfo ban_list[800000];
int site_cnt;

void usage() {
    printf("syntax : 1m-block <site list .txt file>\n");
	printf("sample : sample : 1m-block top-1m.txt\n");
}

unsigned long int hash(char *url, int len, int index)
{
	unsigned long int val = 401;
	for(int i = 0; i < len; i++) {
		val = (val << (index + 4) + (int)url[i]) % HASH_MOD;
	}

	return val;
}

int hash_check(char *host, int host_len, int site_index) 
{
	int flag = TRUE;
	for(int i = 0; i < 2; i++) {
		if(ban_list[site_index].urlhash[i] != hash(host, host_len, i)) {
			flag = FALSE;
			break;
		}
	}

	return flag;
}

void dump(unsigned char* buf, int size) {
	int i;
	for (i = 0; i < size; i++) {
		if (i % 16 == 0)
			printf("\n");
		printf("%02x ", buf[i]);
	}
}

/* returns packet id */
static u_int32_t print_pkt (struct nfq_data *tb)
{
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	u_int32_t mark,ifi;
	int ret;
	unsigned char *data;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
		//printf("hw_protocol=0x%04x hook=%u id=%u ",
			//ntohs(ph->hw_protocol), ph->hook, id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);

		//printf("hw_src_addr=");
		//for (i = 0; i < hlen-1; i++)
			//printf("%02x:", hwph->hw_addr[i]);
		//printf("%02x ", hwph->hw_addr[hlen-1]);
	}

	mark = nfq_get_nfmark(tb);
	//if (mark)
		//printf("mark=%u ", mark);

	ifi = nfq_get_indev(tb);
	//if (ifi)
		//printf("indev=%u ", ifi);

	ifi = nfq_get_outdev(tb);
	//if (ifi)
		//printf("outdev=%u ", ifi);
	ifi = nfq_get_physindev(tb);
	//if (ifi)
		//printf("physindev=%u ", ifi);

	ifi = nfq_get_physoutdev(tb);
	//if (ifi)
		//printf("physoutdev=%u ", ifi);

	ret = nfq_get_payload(tb, &data);
	//if (ret >= 0) 
		//printf("payload_len=%d ", ret);
	
	// Check the header length.
	int iphdr_len = (data[0] & 0x0F) * 4;
	int tcphdr_len = ((data[iphdr_len + 12] & 0xF0) >> 4) * 4;
	filter_flag = FALSE;

	// HTTP check!
	int http_flag = FALSE;
	for(int i = 0; i < 6; i++) {
		if(!memcmp(data + iphdr_len + tcphdr_len, http_method[i], strlen(http_method[i]))) {
			http_flag = TRUE;
			break;
		}
	}

	// Host check!
	if (http_flag) {
		int index = 0;
		while(1) {
			if (!memcmp(data + iphdr_len + tcphdr_len + index, "Host: ", 6)) break;
			index += 1;
		}
		int host_index = 0;
		char hosturl[90];
		while(1) {
			hosturl[host_index] = *(data + iphdr_len + tcphdr_len + index + 6 + host_index);
			if(!memcmp(data + iphdr_len + tcphdr_len + index + 6 + host_index, "\x0d\x0a", 2)) break;
			host_index += 1;
		}
		
		for(int i = 0; i < site_cnt; i++){
			if(host_index != ban_list[i].len) continue;
			if(hosturl[0] != ban_list[i].firstch) continue;
			if(hash_check(hosturl, host_index, i)) {
				filter_flag = TRUE;
				printf("Blocked! %s\n", hosturl);
				break;
			}
		}
	}
		
	//fputc('\n', stdout);

	return id;
}


static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *data)
{
	u_int32_t id = print_pkt(nfa);
	//printf("entering callback\n");
	if(filter_flag) return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	else return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
}

int main(int argc, char **argv)
{
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	struct nfnl_handle *nh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));
	
	if(argc != 2) {
		usage();
		return -1;
	}

	FILE *fp = fopen(argv[1], "r");
	site_cnt = 0;
	while(1) {
        char buffer[90];
        if(fgets(buffer, sizeof(buffer), fp) == NULL) break;
        ban_list[site_cnt].len = (int)strlen(buffer) - 2; // eliminate null byte
        ban_list[site_cnt].firstch = buffer[0];
        for(int i = 0; i < 2; i++) ban_list[site_cnt].urlhash[i] = hash(buffer, ban_list[site_cnt].len, i);
        site_cnt++;
    }

	printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	printf("binding this socket to queue '0'\n");
	qh = nfq_create_queue(h,  0, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			//printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}
		/* if your application is too slow to digest the packets that
		 * are sent from kernel-space, the socket buffer that we use
		 * to enqueue packets may fill up returning ENOBUFS. Depending
		 * on your application, this error may be ignored. nfq_nlmsg_verdict_putPlease, see
		 * the doxygen documentation of this library on how to improve
		 * this situation.
		 */
		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	fclose(fp);

	exit(0);
}
