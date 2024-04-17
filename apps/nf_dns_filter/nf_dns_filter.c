#include <stdio.h>
#include <stdlib.h>
// #include <regex.h>
// #include <arpa/inet.h>
// #include <linux/kernel.h>
// #include <linux/module.h>
// #include <linux/netfilter.h>
// #include <linux/netfilter_ipv4.h>
// #include <linux/if_ether.h>
// #include <linux/ip.h>
// #include <linux/udp.h>
// #include <net/skbuff.h>
#if 0
#define MAX_RULES 100
#define MAX_REGEX_LENGTH 256

regex_t compiled_rules[MAX_RULES];
int rule_count = 0;

int load_regex_rules() {
    FILE *file = fopen("/home/ubuntu/Nutcracker/apps/nf_dns_filter/rule.conf", "r");
    if (!file) {
        perror("Failed to open file");
        return -1;
    }

    char regex[MAX_REGEX_LENGTH];
    int ret;
    while (fgets(regex, MAX_REGEX_LENGTH, file)) {
        if (regex[strlen(regex) - 1] == '\n') {
            regex[strlen(regex) - 1] = '\0';  // Remove newline character
        }

        printf("Regex rule: %s\n", regex);

        ret = regcomp(&compiled_rules[rule_count], regex, REG_EXTENDED);
        if (ret) {
            fprintf(stderr, "Could not compile regex: %s\n", regex);
            continue;
        }
        rule_count++;
        if (rule_count >= MAX_RULES) break;
    }

    fclose(file);
    return 0;
}
#endif
#if 0
static unsigned int check_cond(struct sk_buff *skb) {
    struct ethhdr * ethhdr;
    struct iphdr * iphdr;
    uint16_t iphdr_hlen;
    uint8_t iphdr_protocol;
    struct udphdr * udphdr;

    ethhdr = (struct ethhdr *)skb->ptr;

    iphdr = (struct iphdr *)&ethhdr[1];
    iphdr_hlen = iphdr->ihl;
    iphdr_hlen <<= 2;
    iphdr_protocol = iphdr->protocol;

    if (iphdr_protocol != IPPROTO_UDP) return NF_MISS;

    udphdr = (struct udphdr *)((uint8_t *)iphdr + iphdr_hlen);
    if (ntohs(udphdr->dest) != 53) return NF_MISS;

    return NF_MATCH; // Accept the packet
}

struct dns_header {
    uint16_t id; // Transaction ID
    uint16_t flags; // DNS flags
    uint16_t qdcount; // Number of questions
    uint16_t ancount; // Number of answers
    uint16_t nscount; // Number of authority records
    uint16_t arcount; // Number of additional records
};

// Function to print a domain name from a DNS query
void print_domain_name(const unsigned char * buffer, int* position, unsigned char * domain_name) {
    int len = buffer[(*position)++];
    while (len > 0) {
        for (int i = 0; i < len; i++) {
            *(domain_name++) = buffer[(*position)++];
        }
        len = buffer[(*position)++];
        if (len > 0) {
            *(domain_name++) = '.';
        }
    }
}

// Parse and print details from a DNS query
void parse_dns_query(const unsigned char* buffer, int size) {
    unsigned char domain_name[256] = {0};
    if (size < sizeof(struct dns_header)) {
        printf("Buffer too small for DNS header\n");
        return;
    }
    // Cast the buffer to the DNS header struct
    struct dns_header* dns = (struct dns_header*)buffer;
    int position = sizeof(struct dns_header); // Position in the buffer
    print_domain_name(buffer, &position, domain_name);
}

// Function to be called by hook
static unsigned int hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct ethhdr * ethhdr;
    struct iphdr * iphdr;
    uint16_t iphdr_hlen;
    uint8_t iphdr_protocol;
    struct udphdr * udphdr;
	uint16_t ulen, len;
	uint8_t * data;

    ethhdr = (struct ethhdr *)skb->ptr;

    iphdr = (struct iphdr *)&ethhdr[1];
    iphdr_hlen = iphdr->ihl;
    iphdr_hlen <<= 2;

    udphdr = (struct udphdr *)((uint8_t *)iphdr + iphdr_hlen);
    ulen = ntohs(udphdr->len);
	len = ulen - sizeof(struct udphdr);
	data = (uint8_t *)udphdr + sizeof(struct udphdr);

    // parse_dns_query(data, len);

    return NF_MATCH; // Accept the packet
}

static struct nf_hook_ops nfho = {
    .cond       = check_cond,
    .hook       = hook_func,
    .hooknum    = NF_INET_PRE_ROUTING,
    .pf         = NFPROTO_INET,
};

int nf_dns_filter_init(void) {
    // if (load_regex_rules() != 0) {
    //     return -1;
    // }
    nf_register_net_hook(NULL, &nfho);
    return 0;
}
#endif
int nf_dns_filter_init(void) {
    // regex_t regex;
    // int result;
    // char *pattern = "^[a-zA-Z]+[0-9]*$";  // Regex pattern: one or more letters followed by zero or more numbers
    // char *test_string = "Hello123";

    // // Compile the regular expression
    // result = regcomp(&regex, pattern, REG_EXTENDED);
    // if (result) {
    //     fprintf(stderr, "Could not compile regex\n");
    //     exit(1);
    // }

    // // Execute regular expression
    // result = regexec(&regex, test_string, 0, NULL, 0);
    // if (!result) {
    //     printf("'%s' matches the pattern '%s'\n", test_string, pattern);
    // } else if (result == REG_NOMATCH) {
    //     printf("'%s' does not match the pattern '%s'\n", test_string, pattern);
    // } else {
    //     char error_message[100];
    //     regerror(result, &regex, error_message, sizeof(error_message));
    //     fprintf(stderr, "Regex match failed: %s\n", error_message);
    //     exit(1);
    // }

    // // Free memory allocated to the pattern buffer by regcomp
    // regfree(&regex);

    return 0;
}
