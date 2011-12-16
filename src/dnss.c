#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <getopt.h>
#include <features.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "headers.c"

int main(int argc, char **argv) {
		
	int opt;
	int redirect = 0;
	int map = 0;
	int option_ctr = 0;
	int listener_pid = 0;
	int sender_pid = 0;
	int semid = 0;
	
	char *direction_target = (char *)calloc(SIZE_MAX_FILEN, 1);
	char *interface = (char *)calloc(IFNAMSIZ, 1);
	char *target = (char *)calloc(SIZE_IP_STR, 1);
	char *redirect_target = (char *)calloc(SIZE_IP_STR, 1);
	char *map_file_name = (char *)calloc(SIZE_MAX_FILEN, 1);  

	key_t key;

	if (geteuid() != 0) {
		fprintf(stderr, "dnss requires root permissions!\n");
		exit(EXIT_FAILURE);
	}
	
	while ((opt = getopt(argc, argv, "i:t:rm")) != -1) {
		switch (opt) {
			case 'i':
				strncpy(interface, optarg, SIZE_MAX_NETN);
				break;
			case 't':
				strncpy(target, optarg, SIZE_IP_STR);
				break;
			case 'r':
				if (map == 1) {
					print_usage();
				}
				redirect = 1;
				break;
			case 'm':
				if (redirect == 1) {	
					print_usage();	
				}
				map = 1;
				break;
		}
		option_ctr++;
	}
	
	if (option_ctr != 3 || argc != 7) {
		print_usage();
	}
		
	if (optind < argc) {	
		if (redirect) { 
			strncpy(direction_target, argv[optind], SIZE_IP_STR);		
		}else if (map) {
			strncpy(direction_target, argv[optind], SIZE_MAX_FILEN);
		}
	}

	if ((key = ftok("dnss.c", 'C')) == -1) {
		perror("Error creating key with ftok");
		exit(EXIT_FAILURE);
	}
	
   if ((semid = init_semaphores(key)) == -1) {
		exit(EXIT_FAILURE);
	}
	/* fork a listener and a sender. listener always listens for dns queries
		and writes to sender process queue. sender reads count packets	*/
	switch (listener_pid = fork()) {
		case 0:
			return dns_listener(interface, target);
			break;
		case -1:
			fprintf(stderr, "Error forking listener\n");
			exit(EXIT_FAILURE);
			break;
		default:
			switch (sender_pid = fork()) {
				case 0:
					return dns_sender();
					break;
				case -1:
					fprintf(stderr, "Error forking sender\n");
					exit(EXIT_FAILURE);			
					break;
				default:
				break;
			}
			break;
	}
	
	//Make sure children have time to install PDEATHSIG handler
	sleep(1);

	//If one child exits before the other, kill the remaining child
	waitpid(-1, 0, 0);
	if (waitpid(listener_pid, 0, WNOHANG) == 0) {
		fprintf(stderr, "Killing sender process and exiting\n");
		kill(listener_pid, SIGKILL);
	}

	if (waitpid(sender_pid, 0, WNOHANG) == 0) {
		fprintf(stderr, "Killing listener process and exiting\n");
		kill(sender_pid, SIGKILL);
	}

	free(direction_target);
	free(interface);
	free(target);
	free(redirect_target);
	free(map_file_name);

	exit(EXIT_SUCCESS);
}

int init_semaphores(key_t key) {
	int i;
	int semid;
	union semun arg;
	struct semid_ds buf;
	struct sembuf sb;	

	/* Create semaphores for bounded buffer */
	if ((semid = semget(key, NUM_SEM, IPC_CREAT | IPC_EXCL | 0666)) == -1) {
		perror("Semget");
		exit(EXIT_FAILURE);
	}

	for (sb.sem_num = 0; sb.sem_num < NUM_SEM; sb.sem_num++) {
		/* Free ALL the semaphores! */
		if (semop(semid, &sb, 1) == -1) {
			perror("Error freeing semaphores");	
			semctl(semid, 0, IPC_RMID);
			return -1;	
		}
	}

	return semid;
}

void print_usage() {
	fprintf(stderr, "dnss -i interface -t target -r|m target|file\n");
	exit(EXIT_FAILURE);
}

void sigproc(int signo) {
	exit(EXIT_FAILURE);
}

/* Will listen for DNS requests matching a criteria 
	and write to shared buffer */
int dns_listener(char *interface, char *target_ip) {
	
   int sockfd;
   struct sockaddr_ll saddr;
	struct ifreq ifr;
	struct ip_header *ip;
	struct udp_header *udp;

	unsigned char *pktBuf = (unsigned char *)calloc(MAX_PACKET_LEN, 1);	
	
	signal(SIGHUP, sigproc);	

	bzero(&saddr, sizeof(saddr));
	bzero(&ifr, sizeof(ifr));

	if (prctl(PR_SET_PDEATHSIG, SIGHUP) != 0) {
		fprintf(stderr, "Unable to install parent death sig\n");
	}

   if ((sockfd = socket (PF_PACKET, SOCK_RAW, 0)) == -1) {
      perror("Error creating listening socket\n");
      free(pktBuf);
      exit(EXIT_FAILURE);
   }

	strncpy((char *)ifr.ifr_name, interface, IFNAMSIZ);
	if ((ioctl(sockfd, SIOCGIFINDEX, &ifr)) < 0) {
		fprintf(stderr, "Unable to get interface index!\n");
		exit(EXIT_FAILURE);
	} 

	//Bind the socket to the specified interface
	saddr.sll_family = AF_PACKET;
	saddr.sll_ifindex = ifr.ifr_ifindex;
	saddr.sll_protocol = htons(ETH_P_IP);

	if ((bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr))) == -1) {
		perror("Error binding raw socket to interface\n");
		exit(EXIT_FAILURE);
	}
	
	strncpy((char *)ifr.ifr_name, interface, IFNAMSIZ);
	 
	while (recvfrom(sockfd, pktBuf, 8192, 0, 0, 0) > 0) {
		int ip_len;

		ip = (struct ip_header *)(pktBuf + sizeof(struct eth_header));
		ip_len = 4 * ((ip->ver) & 0x0f);		
		
		//Change to dport when done testing in switched environment
		if (ip->protocol == (char)PROTO_UDP && !compare_ip(target_ip, ip->sip)) {
			udp = (struct udp_header *)(pktBuf + sizeof(struct eth_header) + ip_len);
			if (htons(udp->sport) == PORT_DNS) {
				printf("UDP sport: %hu, dport: %hu\n", ntohs(udp->sport), ntohs(udp->dport));
			}
		}
	}

	printf("listener done");
	
   free(pktBuf);
	close(sockfd);
   return 0;
}

int compare_ip(char *target, u_char *cur_ip) {
	int i;
	char *cur_ip_str = (char *)calloc(SIZE_IP_STR, 1);
	char *tmp = (char *)calloc(SIZE_IP_STR, 1);

	for (i=0; i<4; i++) {
		sprintf(tmp, "%d", cur_ip[i]);
		strcat(cur_ip_str, tmp);
		if (i != 3) 
			strcat(cur_ip_str, ".");
		memset(tmp, 0, SIZE_IP_STR);
	}
	
	i = strncmp(cur_ip_str, target, strlen(target));
	free(cur_ip_str);	
	free(tmp);
	return i;
}

/* Reads dns packets from buffer and respond */
int dns_sender() {

	signal(SIGHUP, sigproc);	
	prctl(PR_SET_PDEATHSIG, SIGHUP);

	while(1) {
	}

	return 0;
}

void print_buf(u_char *pkt) {
	int i = sizeof(struct eth_header);

	printf("%d\n", i );
	
	for (i = 0; i < 100; i++) {
		printf("%x ", pkt[i]);	
	}
	printf("\n");
}

/* */
int build_dns() {
	return 0;
}

int send_dns() {
	return 0;
}

