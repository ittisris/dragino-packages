/**
 * Author: Dragino 
 * Date: 16/01/2018
 * 
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.dragino.com
 *
 * 
*/

/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
	#define _XOPEN_SOURCE 600
#else
	#define _XOPEN_SOURCE 500
#endif

#include <signal.h>		/* sigaction */
#include <errno.h>		/* error messages */

#include <sys/socket.h> /* socket specific definitions */
#include <netinet/in.h> /* INET constants and stuff */
#include <arpa/inet.h>  /* IP address conversion stuff */
#include <netdb.h>		/* gai_strerror */
#include <mosquitto.h>		/* gai_strerror */

#include <uci.h>

#include "radio.h"
#include "jitqueue.h"
#include "parson.h"
#include "base64.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE CONSTANTS ---------------------------------------------------- */

#ifndef VERSION_STRING
  #define VERSION_STRING "undefined"
#endif

#define DEFAULT_KEEPALIVE	5	/* default time interval for downstream keep-alive packet */
#define DEFAULT_STAT		30	/* default time interval for statistics */
#define PUSH_TIMEOUT_MS		100
#define PULL_TIMEOUT_MS		200
#define FETCH_SLEEP_MS		10	/* nb of ms waited when a fetch return no packets */

#define	PROTOCOL_VERSION	2

#define PKT_PUSH_DATA	0
#define PKT_PUSH_ACK	1
#define PKT_PULL_DATA	2
#define PKT_PULL_RESP	3
#define PKT_PULL_ACK	4
#define PKT_TX_ACK      5

#define NB_PKT_MAX		8 /* max number of packets per fetch/send cycle */

#define MIN_LORA_PREAMB	6 /* minimum Lora preamble length for this application */
#define STD_LORA_PREAMB	8
#define MIN_FSK_PREAMB	3 /* minimum FSK preamble length for this application */
#define STD_FSK_PREAMB	4

#define TX_BUFF_SIZE	((540 * NB_PKT_MAX) + 30)
#define STATUS_SIZE	    1024

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES (GLOBAL) ------------------------------------------- */

/* pkt struct */
static struct pkt_rx_s pktrx[QUEUESIZE]; /* allocat queuesize struct of pkt_rx_s */

static int pt = 0, prev = 0;  /* pt is point of receive packet postion,  prev is point of process packet thread*/

/* signal handling variables */
volatile bool exit_sig = false; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile bool quit_sig = false; /* 1 -> application terminates without shutting down the hardware */

/* packets filtering configuration variables */
static bool fwd_valid_pkt = true; /* packets with PAYLOAD CRC OK are forwarded */
static bool fwd_error_pkt = false; /* packets with PAYLOAD CRC ERROR are NOT forwarded */
static bool fwd_nocrc_pkt = false; /* packets with NO PAYLOAD CRC are NOT forwarded */

/* network configuration variables */
static uint64_t lgwm = 0; /* Lora gateway MAC address */
static char provider[16] = "Provider";
static char server[64] = {'\0'}; /* address of the server (host name or IPv4/IPv6) */
static char port[8] = "port"; /* server port for upstream traffic */
static char serv_port_down[8] = "1700"; /* server port for downstream traffic */
static char serv_port_up[8] = "1700"; /* server port for downstream traffic */
static int keepalive_time = DEFAULT_KEEPALIVE; /* send a PULL_DATA request every X seconds, negative = disabled */
static char platform[16] = "LG02/OLG02";  /* platform definition */
static char description[16] = "DESC";                        /* used for free form description */
static char email[32]  = "email";                        /* used for contact email */
static char LAT[16] = "LAT";
static char LON[16] = "LON";
static char gatewayid[64] = "GWID";
static char rxsf[8] = "RXSF";
static char txsf[8] = "TXSF";
static char rxbw[8] = "RXBW";
static char txbw[8] = "TXBW";
static char rxcr[8] = "RXCR";
static char txcr[8] = "TXCR";
static char rxprlen[8] = "RXPRLEN";
static char txprlen[8] = "TXPRLEN";
static char rx_freq[16] = "RXFREQ";            /* rx frequency of radio */
static char tx_freq[16] = "TXFREQ";            /* tx frequency of radio */
static char syncwd1[8] = "SYNCWD";            /* tx frequency of radio */
static char syncwd2[8] = "SYNCWD";            /* tx frequency of radio */
static char logdebug[4] = "DEB";          /* debug info option */
static char server_type[16] = "server_type";          /* debug info option */
static char radio_mode[8] = "mode";          /* debug info option */

/* LOG Level */
int DEBUG_PKT_FWD = 0;
int DEBUG_JIT = 0;
int DEBUG_JIT_ERROR = 0;  
int DEBUG_TIMERSYNC = 0;  
int DEBUG_BEACON = 0;     
int DEBUG_INFO = 0;       
int DEBUG_WARNING = 0;   
int DEBUG_ERROR = 0;    
int DEBUG_GPS = 0;     
int DEBUG_SPI = 0;    
int DEBUG_UCI = 0;   

/* Set location */
static float lat=0.0;
static float lon=0.0;
static int   alt=0;

/* values available for the 'modulation' parameters */
/* NOTE: arbitrary values */
#define MOD_UNDEFINED   0
#define MOD_LORA        0x10
#define MOD_FSK         0x20

/* statistics collection configuration variables */
static unsigned stat_interval = DEFAULT_STAT; /* time interval (in sec) at which statistics are collected and displayed */

/* gateway <-> MAC protocol variables */
static uint32_t net_mac_h; /* Most Significant Nibble, network order */
static uint32_t net_mac_l; /* Least Significant Nibble, network order */

/* network sockets */
static int sock_stat; /* socket for upstream traffic */
static int sock_up; /* socket for upstream traffic */
static int sock_down; /* socket for downstream traffic */

/* network protocol variables */
static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)}; /* cut in half, critical for throughput */
static struct timeval pull_timeout = {0, (PULL_TIMEOUT_MS * 1000)}; /* non critical for throughput */

/* hardware access control and correction */
static pthread_mutex_t mx_concent = PTHREAD_MUTEX_INITIALIZER; /* control access to the concentrator */

/* measurements to establish statistics */
static pthread_mutex_t mx_meas_up = PTHREAD_MUTEX_INITIALIZER; /* control access to the upstream measurements */
static uint32_t meas_nb_rx_rcv = 0; /* count packets received */
static uint32_t meas_nb_rx_ok = 0; /* count packets received with PAYLOAD CRC OK */
static uint32_t meas_nb_rx_bad = 0; /* count packets received with PAYLOAD CRC ERROR */
static uint32_t meas_nb_rx_nocrc = 0; /* count packets received with NO PAYLOAD CRC */
static uint32_t meas_up_pkt_fwd = 0; /* number of radio packet forwarded to the server */
static uint32_t meas_up_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_up_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_up_dgram_sent = 0; /* number of datagrams sent for upstream traffic */
static uint32_t meas_up_ack_rcv = 0; /* number of datagrams acknowledged for upstream traffic */

static pthread_mutex_t mx_meas_dw = PTHREAD_MUTEX_INITIALIZER; /* control access to the downstream measurements */
static uint32_t meas_dw_pull_sent = 0; /* number of PULL requests sent for downstream traffic */
static uint32_t meas_dw_ack_rcv = 0; /* number of PULL requests acknowledged for downstream traffic */
static uint32_t meas_dw_dgram_rcv = 0; /* count PULL response packets received for downstream traffic */
static uint32_t meas_dw_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_dw_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_nb_tx_ok = 0; /* count packets emitted successfully */
static uint32_t meas_nb_tx_fail = 0; /* count packets were TX failed for other reasons */
static uint32_t meas_nb_tx_requested = 0; /* count TX request from server (downlinks) */
static uint32_t meas_nb_tx_rejected_collision_packet = 0; /* count packets were TX request were rejected due to collision with anothe  r packet already programmed */
static uint32_t meas_nb_tx_rejected_collision_beacon = 0; /* count packets were TX request were rejected due to collision with a beac  on already programmed */
static uint32_t meas_nb_tx_rejected_too_late = 0; /* count packets were TX request were rejected because it is too late to program it   */
static uint32_t meas_nb_tx_rejected_too_early = 0; /* count packets were TX request were rejected because timestamp is too much in ad  vance */

/* auto-quit function */
static uint32_t autoquit_threshold = 0; /* enable auto-quit after a number of non-acknowledged PULL_DATA (0 = disabled)*/

/* Just In Time TX scheduling */
static struct jit_queue_s jit_queue;

/* mqtt publish */
/*
static bool mqttconnect = true ;

static struct mqtt_config mconf = {
	.id = "client_id",
	.keepalive = 300,
	.host = "server",
	.port = "port",
	.qos = 1,
	.topic = "topic_format", 
	.data_format = "data_format", 
	.username = "username",
	.password = "password",
        .clean_session = true,
};
*/

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DECLARATION ---------------------------------------- */

static void sig_handler(int sigio);

static char uci_config_file[32] = {'\0'};
static struct uci_context * ctx = NULL; 
static bool get_config(const char *section, char *option, int len);

static double difftimespec(struct timespec end, struct timespec beginning);

static void wait_ms(unsigned long a); 

//static int my_publish(struct mqtt_config *);

/* radio devices */

radiodev *rxdev;
radiodev *txdev;

/* threads */
void thread_stat(void);
void thread_up(void);
void thread_down(void);
void thread_jit(void);

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

static void sig_handler(int sigio) {
    if (sigio == SIGQUIT) {
	    quit_sig = true;;
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
	    exit_sig = true;
    }
    return;
}

static bool get_config(const char *section, char *option, int len) {
    struct uci_package * pkg = NULL;
    struct uci_element *e;
    const char *value;
    bool ret = false;

    ctx = uci_alloc_context(); 
    if (UCI_OK != uci_load(ctx, uci_config_file, &pkg))  
        goto cleanup;   /* load uci conifg failed*/

    uci_foreach_element(&pkg->sections, e)
    {
        struct uci_section *st = uci_to_section(e);

        if(!strcmp(section, st->e.name))  /* compare section name */ {
            if (NULL != (value = uci_lookup_option_string(ctx, st, option))) {
	         memset(option, 0, len);
                 strncpy(option, value, len); 
                 ret = true;
                 break;
            }
        }
    }
    uci_unload(ctx, pkg); /* free pkg which is UCI package */
cleanup:
    uci_free_context(ctx);
    ctx = NULL;
    return ret;
}

static double difftimespec(struct timespec end, struct timespec beginning) {
    double x;
    
    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);
    
    return x;
}

static void wait_ms(unsigned long a) {
    struct timespec dly;
    struct timespec rem;

    dly.tv_sec = a / 1000;
    dly.tv_nsec = ((long)a % 1000) * 1000000;

    //MSG("NOTE dly: %ld sec %ld ns\n", dly.tv_sec, dly.tv_nsec);

    if((dly.tv_sec > 0) || ((dly.tv_sec == 0) && (dly.tv_nsec > 100000))) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, &dly, &rem);
        //MSG("NOTE remain: %ld sec %ld ns\n", rem.tv_sec, rem.tv_nsec);
    }
    return;
}

static struct pkt_rx_s *pkt_alloc(void) {
    return (struct pkt_rx_s *) malloc(sizeof(struct pkt_rx_s));
}

static void pktrx_clean(struct pkt_rx_s *rx) {
    rx->empty = 1;
    rx->size = 0;
    memset(rx->payload, 0, sizeof(rx->payload));
}

static int send_tx_ack(uint8_t token_h, uint8_t token_l, enum jit_error_e error) {
    uint8_t buff_ack[64]; /* buffer to give feedback to server */
    int buff_index;

    /* reset buffer */
    memset(&buff_ack, 0, sizeof buff_ack);

    /* Prepare downlink feedback to be sent to server */
    buff_ack[0] = PROTOCOL_VERSION;
    buff_ack[1] = token_h;
    buff_ack[2] = token_l;
    buff_ack[3] = PKT_TX_ACK;
    *(uint32_t *)(buff_ack + 4) = net_mac_h;
    *(uint32_t *)(buff_ack + 8) = net_mac_l;
    buff_index = 12; /* 12-byte header */

    /* Put no JSON string if there is nothing to report */
    if (error != JIT_ERROR_OK) {
        /* start of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"{\"txpk_ack\":{", 13);
        buff_index += 13;
        /* set downlink error status in JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"\"error\":", 8);
        buff_index += 8;
        switch (error) {
            case JIT_ERROR_FULL:
            case JIT_ERROR_COLLISION_PACKET:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_PACKET\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_collision_packet += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TOO_LATE:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_LATE\"", 10);
                buff_index += 10;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_too_late += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TOO_EARLY:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_EARLY\"", 11);
                buff_index += 11;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_too_early += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_COLLISION_BEACON:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_BEACON\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&mx_meas_dw);
                meas_nb_tx_rejected_collision_beacon += 1;
                pthread_mutex_unlock(&mx_meas_dw);
                break;
            case JIT_ERROR_TX_FREQ:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_FREQ\"", 9);
                buff_index += 9;
                break;
            case JIT_ERROR_TX_POWER:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_POWER\"", 10);
                buff_index += 10;
                break;
            case JIT_ERROR_GPS_UNLOCKED:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"GPS_UNLOCKED\"", 14);
                buff_index += 14;
                break;
            default:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"UNKNOWN\"", 9);
                buff_index += 9;
                break;
        }
        /* end of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"}}", 2);
        buff_index += 2;
    }

    buff_ack[buff_index] = 0; /* add string terminator, for safety */

    /* send datagram to server */
    return send(sock_down, (void *)buff_ack, buff_index, 0);
}

/* -------------------------------------------------------------------------- */
/* --- MQTT FUNCTION -------------------------------------------------------- */
/*
static void my_connect_callback(struct mosquitto *mosq, void *obj, int result)
{
    if (result) 
        fprintf(stderr, "ERROR %s\n", mosquitto_connack_string(result));
}

static void my_disconnect_callback(struct mosquitto *mosq, void *obj, int result)
{
    mqttconnect = false;
}

static void my_publish_callback(struct mosquitto *mosq, void *obj, int mid)
{
    mosquitto_disconnect((struct mosquitto *)obj);
}

static int my_publish(struct mqtt_config *conf)
{
	struct mosquitto *mosq;

	mosquitto_lib_init();

	mosq = mosquitto_new(conf->id, true, NULL);
	mosquitto_connect_callback_set(mosq, my_connect_callback);
	mosquitto_disconnect_callback_set(mosq, my_disconnect_callback);
	mosquitto_publish_callback_set(mosq, my_publish_callback);

        mosquitto_username_pw_set(mosq, conf->username, conf->password);

	mosquitto_connect(mosq, conf->host, atoi(conf->port), conf->keepalive);

	mosquitto_loop_start(mosq);

        mosquitto_publish(mosq, NULL, conf->topic, conf->msglen, conf->message, conf->qos, false);

	mosquitto_loop_stop(mosq, false);

	mosquitto_destroy(mosq);

	mosquitto_lib_cleanup();

	return 0;
}
*/

/* -------------------------------------------------------------------------- */
/* --- MAIN FUNCTION -------------------------------------------------------- */

int main(int argc, char *argv[])
{
    struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */
    int i; /* loop variable and temporary variable for return value */

    FILE *fp = NULL;
    
    /* threads */
    pthread_t thrid_stat;
    pthread_t thrid_up;
    pthread_t thrid_down;
    pthread_t thrid_jit;

    /* network socket creation */
    struct addrinfo hints;
    struct addrinfo *result; /* store result of getaddrinfo */
    struct addrinfo *q; /* pointer to move into *result data */
    char host_name[64];
    char port_name[64];
    
    unsigned long long ull = 0;

    /* display version informations */
    //MSG("*** Basic Packet Forwarder for LG02 ***\nVersion: " VERSION_STRING "\n");
    //
    // Make sure only one copy of the daemon is running.
    if (already_running()) {
        MSG_LOG(DEBUG_ERROR, "ERROR~ %s: already running!\n", argv[0]);
        exit(1);
    }

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
    /* load configuration */
    strcpy(uci_config_file, "/etc/config/gateway");

    if (!get_config("general", provider, 16)){
        strcpy(provider, "ttn");  
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option provider=%s\n", provider);
    }

    snprintf(server, sizeof(server), "%s_server", provider); 

    if (!get_config("general", server, sizeof(server))){ /*set default:router.eu.thethings.network*/
        strcpy(server, "router.us.thethings.network");  
    }

    /*
    if (!get_config("general", ttn_s, 64)){
        strcpy(ttn_s, "router.us.thethings.network");  
    }
    */

    if (!get_config("general", port, 8)){
        strcpy(port, "1700");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option port=%s\n", port);
    }

    strcpy(serv_port_up, port);
    strcpy(serv_port_down, port);

    if (!get_config("general", email, 32)){
        strcpy(email, "dragino@dragino.com");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option email=%s\n", email);
    }

    if (!get_config("general", gatewayid, 64)){
        strcpy(gatewayid, "a84041ffff16c21c");      /*set default:router.eu.thethings.network*/
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option gatewayid=%s\n", gatewayid);
    } 

    if (!get_config("general", LAT, 16)){
        strcpy(LAT, "0");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option lat=%s\n", LAT);
    }

    if (!get_config("general", LON, 16)){
        strcpy(LON, "0");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option lon=%s\n", LON);
    }

    if (!get_config("general", logdebug, 4)){
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option logdebug=%s\n", logdebug);
    }

    /* server_type : 1.lorawan  2.relay  3.mqtt  4.tcpudp */
    if (!get_config("general", server_type, 16)){
        strcpy(server_type, "lorawan");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option server_type=%s\n", server_type);
    }

    /* mode0.A for RX, B for TX mode1.B for RX, A for TX mode2. both for RX, no TX */
    if (!get_config("general", radio_mode, 8)){
        strcpy(radio_mode, "0");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option radio_mode=%s\n", radio_mode);
    }

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

    if (!get_config("radio1", rx_freq, 16)){
        strcpy(rx_freq, "902320000"); /* default frequency*/
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option rxfreq=%s\n", rx_freq);
    }

    if (!get_config("radio1", rxsf, 8)){
        strcpy(rxsf, "7");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option rxsf=%s\n", rxsf);
    }

    if (!get_config("radio1", rxcr, 8)){
        strcpy(rxcr, "5");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option coderate=%s\n", rxcr);
    }
    
    if (!get_config("radio1", rxbw, 8)){
        strcpy(rxbw, "125000");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option rxbw=%s\n", rxbw);
    }

    if (!get_config("radio1", rxprlen, 8)){
        strcpy(rxprlen, "8");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option rxprlen=%s\n", rxprlen);
    }

    if (!get_config("radio1", syncwd1, 8)){
        strcpy(syncwd1, "52");  //Value 0x34 is reserved for LoRaWAN networks
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option syncword=0x%02x\n", syncwd1);
    }

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

    if (!get_config("radio2", tx_freq, 16)){
        strcpy(tx_freq, "923300000"); /* default frequency*/
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option txfreq=%s\n", tx_freq);
    }

    if (!get_config("radio2", txsf, 8)){
        strcpy(txsf, "9");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option txsf=%s\n", txsf);
    }

    if (!get_config("radio2", txcr, 8)){
        strcpy(txcr, "5");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option coderate=%s\n", txcr);
    }

    if (!get_config("radio2", txbw, 8)){
        strcpy(txbw, "125000");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option rxbw=%s\n", txbw);
    }

    if (!get_config("radio2", txprlen, 8)){
        strcpy(txprlen, "8");
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option txprlen=%s\n", txprlen);
    }

    if (!get_config("radio2", syncwd2, 8)){
        strcpy(syncwd2, "52");  //Value 0x34 is reserved for LoRaWAN networks
        MSG_LOG(DEBUG_UCI, "UCIINFO~ get option syncword2=0x%02x\n", syncwd2);
    }

    switch (atoi(logdebug)) {
        case 1:
            DEBUG_INFO = 1;       
            DEBUG_PKT_FWD = 1;
            break;
        case 2:
            DEBUG_INFO = 1;       
            DEBUG_PKT_FWD = 1;
            DEBUG_JIT = 1;
            DEBUG_JIT_ERROR = 1;  
            DEBUG_WARNING = 1;
            DEBUG_ERROR = 1;
            break;
        case 3:
            DEBUG_INFO = 1;       
            DEBUG_PKT_FWD = 1;
            DEBUG_JIT = 1;
            DEBUG_JIT_ERROR = 1;  
            DEBUG_TIMERSYNC = 1;
            DEBUG_WARNING = 1;
            DEBUG_ERROR = 1;
            DEBUG_GPS = 1;
            DEBUG_BEACON = 1;
            DEBUG_UCI = 1;
            break;
        default:
            break;
    }

    lat = atof(LAT);
    lon = atof(LON);

    sscanf(gatewayid, "%llx", &ull);
    lgwm = ull;

    /*
    if (!strcmp(server_type, "mqtt")) {
        char mqtt_server_type[32] = "server_type";

        strcpy(uci_config_file, "/etc/config/mqtt");

        if (!get_config("general", mqtt_server_type, sizeof(mqtt_server_type))){
            strcpy(mqtt_server_type, "thingspeak");  
        }

        if (!get_config("general", mconf.username, sizeof(mconf.username))){
            strcpy(mconf.username, "username");  
        }

        if (!get_config("general", mconf.password, sizeof(mconf.password))){
            strcpy(mconf.password, "password");  
        }

        if (!get_config("general", mconf.id, sizeof(mconf.id))){
            strcpy(mconf.id, "client_id");  
        }

        if (!get_config(mqtt_server_type, mconf.host, sizeof(mconf.host))){
            strcpy(mconf.host, "mqtt.thingspeak.com");  
        }

        if (!get_config(mqtt_server_type, mconf.port, sizeof(mconf.port))){
            strcpy(mconf.port, "1883");  
        }

        if (!get_config(mqtt_server_type, mconf.topic, sizeof(mconf.topic))){
            strcpy(mconf.topic, "topic_format");  
        }

        if (!get_config(mqtt_server_type, mconf.data_format, sizeof(mconf.data_format))){
            strcpy(mconf.topic, "topic_data_format");  
        }

        MSG_LOG(DEBUG_UCI, "UCIINFO~ MQTT: host=%s, port=%s, name=%s, password=%s, topic=%s, data_format=%s\n", mconf.host, mconf.port, \
                                        mconf.username, mconf.password, mconf.topic, mconf.data_format);
    }
    */

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

    /* init the queue of receive packets */
    for (i = 0; i < QUEUESIZE; i++) {  
        pktrx_clean(&pktrx[i]);
    }
	
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
    /* radio device init */

    rxdev = (radiodev *) malloc(sizeof(radiodev));

    txdev = (radiodev *) malloc(sizeof(radiodev));
    
    rxdev->nss = 15;
    rxdev->rst = 8;
    rxdev->dio[0] = 7;
    rxdev->dio[1] = 6;
    rxdev->dio[2] = 0;
    rxdev->spiport = lgw_spi_open(SPI_DEV_RX);
    if (rxdev->spiport < 0) { 
        MSG_LOG(DEBUG_ERROR, "ERROR~ open spi_dev_tx error!\n");
        goto clean;
    }
    rxdev->freq = atol(rx_freq);
    rxdev->sf = atoi(rxsf);
    rxdev->bw = atol(rxbw);
    rxdev->cr = atoi(rxcr);
    rxdev->nocrc = 0;  /* crc check */
    rxdev->prlen = atoi(rxprlen);
    rxdev->syncword = atoi(syncwd1);
    rxdev->invertio = 0;
    strcpy(rxdev->desc, "Radio1");

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
    txdev->nss = 24;
    txdev->rst = 23;
    txdev->dio[0] = 22;
    txdev->dio[1] = 20;
    txdev->dio[2] = 0;
    txdev->spiport = lgw_spi_open(SPI_DEV_TX);
    if (txdev->spiport < 0) {
        MSG_LOG(DEBUG_ERROR, "ERROR~ open spi_dev_tx error!\n");
        goto clean;
    }
    txdev->freq = atol(tx_freq);
    txdev->sf = atoi(txsf);
    txdev->bw = atol(txbw);
    txdev->cr = atoi(txcr);
    txdev->nocrc = 0;
    txdev->prlen = atoi(txprlen);
    txdev->syncword = atoi(syncwd2);
    txdev->invertio = 0;
    strcpy(txdev->desc, "Radio2");

    
    /* swap radio1 and radio2 */
    if (!strcmp(radio_mode, "1")) {
        radiodev *tmpdev;
        tmpdev = rxdev;
        rxdev = txdev;
        txdev = tmpdev;
    }

    MSG_LOG(DEBUG_INFO, "INFO~ %s struct: spiport=%d, freq=%ld, sf=%d\n", rxdev->desc, rxdev->spiport, rxdev->freq, rxdev->sf);
    MSG_LOG(DEBUG_INFO, "INFO~ %s struct: spiport=%d, freq=%ld, sf=%d\n", txdev->desc, txdev->spiport, txdev->freq, txdev->sf);

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

    if(get_radio_version(rxdev) == false) 
        goto clean;

    setup_channel(rxdev);

    if (strcmp(server_type, "lorawan"))
        setup_channel(txdev);

    /* configure signal handling */
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigact.sa_handler = sig_handler;
    sigaction(SIGQUIT, &sigact, NULL); /* Ctrl-\ */
    sigaction(SIGINT, &sigact, NULL); /* Ctrl-C */
    sigaction(SIGTERM, &sigact, NULL); /* default "kill" command */

    /* process some of the configuration variables */
    net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm>>32)));
    net_mac_l = htonl((uint32_t)(0xFFFFFFFF &  lgwm  ));

    MSG_LOG(DEBUG_INFO, "Lora Gateway service Mode=%s, gatewayID=%s\n", server_type, gatewayid);

    fp = fopen("/etc/lora/desc", "w+");
    if (NULL != fp) {
        fprintf(fp, "%s struct: spiport=%d, freq=%ld, sf=%d\n", rxdev->desc, rxdev->spiport, rxdev->freq, rxdev->sf);
        fprintf(fp, "%s struct: spiport=%d, freq=%ld, sf=%d\n", txdev->desc, txdev->spiport, txdev->freq, txdev->sf);
        fprintf(fp, "Lora Gateway service Mode=%s, gatewayID=%s, server=%s\n", server_type, gatewayid, server);
        fflush(fp);
        fclose(fp);
    }

    
    /* look for server address w/ upstream port */
    //MSG("Looking for server with upstream port......\n");
    if (!strcmp(server_type, "lorawan")) {
        MSG_LOG(DEBUG_INFO, "INFO~ Start lora packet forward daemon, server = %s, port = %s\n", server, port);
        /* prepare hints to open network sockets */
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET; /* should handle IP v4 or v6 automatically */
        hints.ai_socktype = SOCK_DGRAM;
        i = getaddrinfo(server, serv_port_up, &hints, &result);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [up] getaddrinfo on address %s (PORT %s) returned %s\n", server, serv_port_up, gai_strerror(i));
                exit(EXIT_FAILURE);
        }
        
        /* try to open socket for upstream traffic */
        //MSG("Try to open socket for upstream......\n");
        for (q=result; q!=NULL; q=q->ai_next) {
                sock_up = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
                if (sock_up == -1) continue; /* try next field */
                else break; /* success, get out of loop */
        }
        if (q == NULL) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [up] failed to open socket to any of server %s addresses (port %s)\n", server, serv_port_up);
                i = 1;
                for (q=result; q!=NULL; q=q->ai_next) {
                        getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
                        MSG_LOG(DEBUG_INFO, "INFO~ [up] result %i host:%s service:%s\n", i, host_name, port_name);
                        ++i;
                }
                exit(EXIT_FAILURE);
        }
        
        /* connect so we can send/receive packet with the server only */
        i = connect(sock_up, q->ai_addr, q->ai_addrlen);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [up] connect returned %s\n", strerror(errno));
                exit(EXIT_FAILURE);
        }
        freeaddrinfo(result);

        /* look for server address w/ status port */
        //MSG("loor for server with status port......\n");
        i = getaddrinfo(server, port, &hints, &result);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [up] getaddrinfo on address %s (PORT %s) returned %s\n", server, serv_port_up, gai_strerror(i));
                exit(EXIT_FAILURE);
        }
        /* try to open socket for status traffic */
        for (q=result; q!=NULL; q=q->ai_next) {
                sock_stat = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
                if (sock_stat == -1) continue; /* try next field */
                else break; /* success, get out of loop */
        }
        if (q == NULL) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [stat] failed to open socket to any of server %s addresses (port %s)\n", server, port);
                i = 1;
                for (q=result; q!=NULL; q=q->ai_next) {
                        getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
                        MSG_LOG(DEBUG_INFO, "INFO~ [stat] result %i host:%s service:%s\n", i, host_name, port_name);
                        ++i;
                }
                exit(EXIT_FAILURE);
        }
        
        /* connect so we can send/receive packet with the server only */
        i = connect(sock_stat, q->ai_addr, q->ai_addrlen);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [stat] connect returned %s\n", strerror(errno));
                exit(EXIT_FAILURE);
        }
        freeaddrinfo(result);

        /* look for server address w/ downstream port */
        //MSG("loor for server with downstream port......\n");
        i = getaddrinfo(server, serv_port_down, &hints, &result);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [down] getaddrinfo on address %s (port %s) returned %s\n", server, serv_port_up, gai_strerror(i));
                exit(EXIT_FAILURE);
        }
        
        /* try to open socket for downstream traffic */
        for (q=result; q!=NULL; q=q->ai_next) {
                sock_down = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
                if (sock_down == -1) continue; /* try next field */
                else break; /* success, get out of loop */
        }
        if (q == NULL) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [down] failed to open socket to any of server %s addresses (port %s)\n", server, serv_port_up);
                i = 1;
                for (q=result; q!=NULL; q=q->ai_next) {
                        getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
                        MSG_LOG(DEBUG_INFO, "INFO~ [down] result %i host:%s service:%s\n", i, host_name, port_name);
                        ++i;
                }
                exit(EXIT_FAILURE);
        }
        
        /* connect so we can send/receive packet with the server only */
        i = connect(sock_down, q->ai_addr, q->ai_addrlen);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [down] connect returned %s\n", strerror(errno));
                exit(EXIT_FAILURE);
        }
        freeaddrinfo(result);

        /* spawn threads to report status*/
        i = pthread_create( &thrid_stat, NULL, (void * (*)(void *))thread_stat, NULL);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [main] impossible to create upstream thread\n");
                exit(EXIT_FAILURE);
        }

        /* spawn threads to manage upstream and downstream */
        MSG("spawn threads to manage upsteam and downstream...\n");
        i = pthread_create( &thrid_up, NULL, (void * (*)(void *))thread_up, NULL);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [main] impossible to create upstream thread\n");
                exit(EXIT_FAILURE);
        }

        i = pthread_create( &thrid_down, NULL, (void * (*)(void *))thread_down, NULL);
        if (i != 0) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ [main] impossible to create downstream thread\n");
                exit(EXIT_FAILURE);

        }

        i = pthread_create( &thrid_jit, NULL, (void * (*)(void *))thread_jit, NULL);
        if (i != 0) {
            MSG_LOG(DEBUG_ERROR, "ERROR~ [main] impossible to create JIT thread\n");
            exit(EXIT_FAILURE);
        }
    }

    /* main thread for receive message, then process the message */

    rxlora(rxdev->spiport, RXMODE_SCAN);  /* star lora continue receive mode */
    MSG_LOG(DEBUG_INFO, "Listening at SF%i on %.6lf Mhz. port%i\n", rxdev->sf, (double)(rxdev->freq)/1000000, rxdev->spiport);
    while (!exit_sig && !quit_sig) {
        if(digitalRead(rxdev->dio[0]) == 1) {
            if (pktrx[pt].empty) {
                if (received(rxdev->spiport, &pktrx[pt]) == true) {
                    if (!strcmp(server_type, "relay")) {      // lora relay mode, trunking
                        single_tx(txdev, pktrx[pt].payload, pktrx[pt].size); 
                        pktrx_clean(&pktrx[pt]); 
                    } else if (!strcmp(server_type, "mqtt") || !strcmp(server_type, "tcpudp")) {  // mqtt mode or tcpudp mode for loraRAW
                        char chan_path[32] = {'\0'};

                        sprintf(chan_path, "/var/iot/channels/%c%c%c%c", pktrx[pt].payload[0], pktrx[pt].payload[1], pktrx[pt].payload[2], pktrx[pt].payload[3]);
                        fp = fopen(chan_path, "w+");
                        if ( NULL != fp ) {
                            fwrite(pktrx[pt].payload, sizeof(char), pktrx[pt].size, fp);  
                            fflush(fp);
                            fclose(fp);
                        } else 
                            MSG_LOG(DEBUG_ERROR, "ERROR~ canot open file path: %s\n", chan_path); 

                        pktrx_clean(&pktrx[pt]);
                    }

                    if (++pt >= QUEUESIZE)
                        pt = 0;
                } 
            }
        }
    }

    if (!strcmp(server_type, "lorawan")) {
        pthread_join(thrid_up, NULL);
        pthread_cancel(thrid_stat);
        pthread_cancel(thrid_down); /* don't wait for downstream thread */
        pthread_cancel(thrid_jit); /* don't wait for jit thread */

        /* if an exit signal was received, try to quit properly */
        if (exit_sig) {
            /* shut down network sockets */
            shutdown(sock_stat, SHUT_RDWR);
            shutdown(sock_up, SHUT_RDWR);
            shutdown(sock_down, SHUT_RDWR);
        }
    }

clean:
    free(rxdev);
    free(txdev);
	
    MSG_LOG(DEBUG_INFO, "INFO~ Exiting Lora service program\n");
    exit(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 1: send status to lora server and print report ---------------------- */

void thread_stat(void) {
    /* ------------------------------------------------------------------------------------ */
    /* statistics collection and send status to server */

    int j;

    /* variables to get local copies of measurements */
    uint32_t cp_nb_rx_rcv;
    uint32_t cp_nb_rx_ok;
    uint32_t cp_nb_rx_bad;
    uint32_t cp_nb_rx_nocrc;
    uint32_t cp_up_pkt_fwd;
    uint32_t cp_up_network_byte;
    uint32_t cp_up_payload_byte;
    uint32_t cp_up_dgram_sent;
    uint32_t cp_up_ack_rcv;
    uint32_t cp_dw_pull_sent;
    uint32_t cp_dw_ack_rcv;
    uint32_t cp_dw_dgram_rcv;
    uint32_t cp_dw_network_byte;
    uint32_t cp_dw_payload_byte;
    uint32_t cp_nb_tx_ok;
    uint32_t cp_nb_tx_fail;

    /* statistics variable */
    time_t t;
    char stat_timestamp[24];
    float rx_ok_ratio;
    float rx_bad_ratio;
    float rx_nocrc_ratio;
    float up_ack_ratio;
    float dw_ack_ratio;

    static char status_report[STATUS_SIZE]; /* status report as a JSON object */

    int stat_index=0;

    /* pre-fill the data buffer with fixed fields */
    status_report[0] = PROTOCOL_VERSION;
    status_report[3] = PKT_PUSH_DATA;

    /* fill GEUI  8bytes */
    *(uint32_t *)(status_report + 4) = net_mac_h; 
    *(uint32_t *)(status_report + 8) = net_mac_l; 
        
    while (!exit_sig && !quit_sig) {
                
        /* get timestamp for statistics */
        t = time(NULL);
        strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));
        
        /* access upstream statistics, copy and reset them */
        pthread_mutex_lock(&mx_meas_up);
        cp_nb_rx_rcv       = meas_nb_rx_rcv;
        cp_nb_rx_ok        = meas_nb_rx_ok;
        cp_nb_rx_bad       = meas_nb_rx_bad;
        cp_nb_rx_nocrc     = meas_nb_rx_nocrc;
        cp_up_pkt_fwd      = meas_up_pkt_fwd;
        cp_up_network_byte = meas_up_network_byte;
        cp_up_payload_byte = meas_up_payload_byte;
        cp_up_dgram_sent   = meas_up_dgram_sent;
        cp_up_ack_rcv      = meas_up_ack_rcv;
        meas_nb_rx_rcv = 0;
        meas_nb_rx_ok = 0;
        meas_nb_rx_bad = 0;
        meas_nb_rx_nocrc = 0;
        meas_up_pkt_fwd = 0;
        meas_up_network_byte = 0;
        meas_up_payload_byte = 0;
        meas_up_dgram_sent = 0;
        meas_up_ack_rcv = 0;
        pthread_mutex_unlock(&mx_meas_up);
        if (cp_nb_rx_rcv > 0) {
                rx_ok_ratio = (float)cp_nb_rx_ok / (float)cp_nb_rx_rcv;
                rx_bad_ratio = (float)cp_nb_rx_bad / (float)cp_nb_rx_rcv;
                rx_nocrc_ratio = (float)cp_nb_rx_nocrc / (float)cp_nb_rx_rcv;
        } else {
                rx_ok_ratio = 0.0;
                rx_bad_ratio = 0.0;
                rx_nocrc_ratio = 0.0;
        }
        if (cp_up_dgram_sent > 0) {
                up_ack_ratio = (float)cp_up_ack_rcv / (float)cp_up_dgram_sent;
        } else {
                up_ack_ratio = 0.0;
        }
        
        /* access downstream statistics, copy and reset them */
        pthread_mutex_lock(&mx_meas_dw);
        cp_dw_pull_sent    =  meas_dw_pull_sent;
        cp_dw_ack_rcv      =  meas_dw_ack_rcv;
        cp_dw_dgram_rcv    =  meas_dw_dgram_rcv;
        cp_dw_network_byte =  meas_dw_network_byte;
        cp_dw_payload_byte =  meas_dw_payload_byte;
        cp_nb_tx_ok        =  meas_nb_tx_ok;
        cp_nb_tx_fail      =  meas_nb_tx_fail;
        meas_dw_pull_sent = 0;
        meas_dw_ack_rcv = 0;
        meas_dw_dgram_rcv = 0;
        meas_dw_network_byte = 0;
        meas_dw_payload_byte = 0;
        meas_nb_tx_ok = 0;
        meas_nb_tx_fail = 0;
        pthread_mutex_unlock(&mx_meas_dw);
        if (cp_dw_pull_sent > 0) {
                dw_ack_ratio = (float)cp_dw_ack_rcv / (float)cp_dw_pull_sent;
        } else {
                dw_ack_ratio = 0.0;
        }
        
        /* display a report */
        
        MSG_LOG(DEBUG_INFO, "\nREPORT~ ##### %s #####\n", stat_timestamp);
        MSG_LOG(DEBUG_INFO, "REPORT~ ### [UPSTREAM] ###\n");
        MSG_LOG(DEBUG_INFO, "REPORT~ # RF packets received by concentrator: %u\n", cp_nb_rx_rcv);
        MSG_LOG(DEBUG_INFO, "REPORT~ # CRC_OK: %.2f%%, CRC_FAIL: %.2f%%, NO_CRC: %.2f%%\n", 100.0 * rx_ok_ratio, 100.0 * rx_bad_ratio, 100.0 * rx_nocrc_ratio);
        MSG_LOG(DEBUG_INFO, "REPORT~ # RF packets forwarded: %u (%u bytes)\n", cp_up_pkt_fwd, cp_up_payload_byte);
        MSG_LOG(DEBUG_INFO, "REPORT~ # PUSH_DATA datagrams sent: %u (%u bytes)\n", cp_up_dgram_sent, cp_up_network_byte);
        MSG_LOG(DEBUG_INFO, "REPORT~ # PUSH_DATA acknowledged: %.2f%%\n", 100.0 * up_ack_ratio);
        MSG_LOG(DEBUG_INFO, "REPORT~ ### [DOWNSTREAM] ###\n");
        MSG_LOG(DEBUG_INFO, "REPORT~ # PULL_DATA sent: %u (%.2f%% acknowledged)\n", cp_dw_pull_sent, 100.0 * dw_ack_ratio);
        MSG_LOG(DEBUG_INFO, "REPORT~ # PULL_RESP(onse) datagrams received: %u (%u bytes)\n", cp_dw_dgram_rcv, cp_dw_network_byte);
        MSG_LOG(DEBUG_INFO, "REPORT~ # RF packets sent to concentrator: %u (%u bytes)\n", (cp_nb_tx_ok+cp_nb_tx_fail), cp_dw_payload_byte);
        MSG_LOG(DEBUG_INFO, "REPORT~ # TX errors: %u\n", cp_nb_tx_fail);
        MSG_LOG(DEBUG_INFO, "REPORT~ ##### END #####\n");
        

        /* start composing datagram with the header */
        uint8_t token_h = (uint8_t)rand(); /* random token */
        uint8_t token_l = (uint8_t)rand(); /* random token */
        status_report[1] = token_h;
        status_report[2] = token_l;

        stat_index = 12; /* 12-byte header */

        /* get timestamp for statistics */
        t = time(NULL);
        strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));

        j = snprintf((char *)(status_report + stat_index), STATUS_SIZE - stat_index, "{\"stat\":{\"time\":\"%s\",\"lati\":%.5f,\"long\":%.5f,\"alti\":%i,\"rxnb\":%u,\"rxok\":%u,\"rxfw\":%u,\"ackr\":%.1f,\"dwnb\":%u,\"txnb\":%u,\"pfrm\":\"%s\",\"mail\":\"%s\",\"desc\":\"%s\"}}", stat_timestamp, lat, lon, (int)alt, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, (float)0, 0, 0, platform, email, description);
        stat_index += j;
        status_report[stat_index] = 0; /* add string terminator, for safety */

        MSG_LOG(DEBUG_INFO, "\nINFO~ (json): [stat update] %s\n", (char *)(status_report + 12)); /* DEBUG: display JSON stat */

        //send the update
        send(sock_stat, (void *)status_report, stat_index, 0);

        /* wait for next reporting interval */
        wait_ms(1000 * stat_interval);
    }
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 2: RECEIVING PACKETS AND FORWARDING THEM ---------------------- */

void thread_up(void) {
    int i, j; /* loop variables */

    /* allocate memory for packet fetching and processing */
    int nb_pkt;
    
    /* local timestamp variables until we get accurate GPS time */
    struct timespec fetch_time;
    struct tm * x;
    char fetch_timestamp[28]; /* timestamp as a text string */
    
    /* data buffers */
    uint8_t buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
    int buff_index;
    uint8_t buff_ack[32]; /* buffer to receive acknowledges */

    /* protocol variables */
    uint8_t token_h; /* random token for acknowledgement matching */
    uint8_t token_l; /* random token for acknowledgement matching */
    
    /* ping measurement variables */
    struct timespec send_time;
    struct timespec recv_time;
    
    /* set upstream socket RX timeout */
    i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO, (void *)&push_timeout_half, sizeof push_timeout_half);
    if (i != 0) {
	    MSG_LOG(DEBUG_ERROR, "ERROR~ [up] setsockopt returned %s\n", strerror(errno));
	    exit(EXIT_FAILURE);
    }
    
    /* pre-fill the data buffer with fixed fields */
    buff_up[0] = PROTOCOL_VERSION;
    buff_up[3] = PKT_PUSH_DATA;
    *(uint32_t *)(buff_up + 4) = net_mac_h;
    *(uint32_t *)(buff_up + 8) = net_mac_l;
    
    while (!exit_sig && !quit_sig) {

        //MSG("INFO~ [up] loop...\n");
                /* fetch packets */

        if (pktrx[prev].empty) {
            if (++prev >= QUEUESIZE)
                prev = 0;

            wait_ms(FETCH_SLEEP_MS); /* wait a short time if no packets */
            continue;
        }

        /* local timestamp generation until we get accurate GPS time */
        clock_gettime(CLOCK_REALTIME, &fetch_time);
        x = gmtime(&(fetch_time.tv_sec)); /* split the UNIX timestamp to its calendar components */
        snprintf(fetch_timestamp, sizeof fetch_timestamp, "%04i-%02i-%02iT%02i:%02i:%02i.%06liZ", (x->tm_year)+1900, (x->tm_mon)+1, x->tm_mday, x->tm_hour, x->tm_min, x->tm_sec, (fetch_time.tv_nsec)/1000); /* ISO 8601 format */
                
        /* get timestamp for statistics */
        struct timeval now;
        gettimeofday(&now, NULL);
        uint32_t tmst = (uint32_t)(now.tv_sec*1000000 + now.tv_usec);

        /* start composing datagram with the header */
        token_h = (uint8_t)rand(); /* random token */
        token_l = (uint8_t)rand(); /* random token */
        buff_up[1] = token_h;
        buff_up[2] = token_l;
        buff_index = 12; /* 12-byte header */

        j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, "{\"rxpk\":[{\"time\":\"%s\",\"tmst\":%u,\"chan\":0,\"rfch\":1,\"freq\":%.6lf,\"stat\":1,\"modu\":\"LORA\",\"datr\":\"SF%dBW125\",\"codr\":\"4/%s\",\"lsnr\":7.8", fetch_timestamp, tmst, (double)(rxdev->freq)/1000000, rxdev->sf, rxcr);

        /*
        j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, "{\"rxpk\":[{\"tmst\":%u,\"chan\":0,\"rfch\":1,\"freq\":%.6lf,\"stat\":1,\"modu\":\"LORA\",\"datr\":\"SF%dBW125\",\"codr\":\"4/%s\",\"lsnr\":7.8", tmst, (double)(rxdev->freq)/1000000, rxdev->sf, rxcr);
        */

        buff_index += j;

        j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"rssi\":%.0f,\"size\":%u", pktrx[prev].rssi, pktrx[prev].size);
                
        buff_index += j;

        memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
                buff_index += 9;
                
        j = bin_to_b64((uint8_t *)pktrx[prev].payload, pktrx[prev].size, (char *)(buff_up + buff_index), 341); /* 255 bytes = 340 chars in b64 + null char */

        buff_index += j;
        buff_up[buff_index] = '"';
        ++buff_index;
        buff_up[buff_index] = '}';
        ++buff_index;
        buff_up[buff_index] = ']';
        ++buff_index;
        buff_up[buff_index] = '}';
        ++buff_index;
        buff_up[buff_index] = '\0'; /* add string terminator, for safety */
                
        MSG_LOG(DEBUG_PKT_FWD, "\nRXTX~ (RXPK): [up] %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */
            
        /* send datagram to server */
        send(sock_up, (void *)buff_up, buff_index, 0);
        clock_gettime(CLOCK_MONOTONIC, &send_time);
        pthread_mutex_lock(&mx_meas_up);
        meas_up_dgram_sent += 1;
        meas_up_network_byte += buff_index;
        
        /* wait for acknowledge (in 2 times, to catch extra packets) */
        for (i=0; i<2; ++i) {
                j = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
                clock_gettime(CLOCK_MONOTONIC, &recv_time);
                if (j == -1) {
                    if (errno == EAGAIN) { /* timeout */
                            continue;
                    } else { /* server connection error */
                            break;
                    }
                } else if ((j < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PUSH_ACK)) {
                    //MSG("WARNING: [up] ignored invalid non-ACL packet\n");
                    continue;
                } else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
                    //MSG("WARNING: [up] ignored out-of sync ACK packet\n");
                    continue;
                } else {
                    MSG_LOG(DEBUG_INFO, "INFO~ [up] PUSH_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
                    meas_up_ack_rcv += 1;
                    break;
                }
        }
        pthread_mutex_unlock(&mx_meas_up);

        /* clean a packet */

        pktrx_clean(&pktrx[prev]);

        if (++prev <= QUEUESIZE)
            prev = 0;

        wait_ms(FETCH_SLEEP_MS); /* wait after receive a packet */
        //MSG("INFO~ [up]return loop\n");
    }
    MSG_LOG(DEBUG_INFO, "\nINFO~ End of upstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 3: POLLING SERVER AND EMITTING PACKETS ------------------------ */

void thread_down(void) {

    int i; /* loop variables */

    /* configuration and metadata for an outbound packet */
    struct pkt_tx_s txpkt;
    bool sent_immediate = false; /* option to sent the packet immediately */
    
    /* local timekeeping variables */
    struct timespec send_time; /* time of the pull request */
    struct timespec recv_time; /* time of return from recv socket call */
    
    /* data buffers */
    uint8_t buff_down[1024]; /* buffer to receive downstream packets */
    uint8_t buff_req[12]; /* buffer to compose pull requests */
    int msg_len;
    
    /* protocol variables */
    uint8_t token_h; /* random token for acknowledgement matching */
    uint8_t token_l; /* random token for acknowledgement matching */
    bool req_ack = false; /* keep track of whether PULL_DATA was acknowledged or not */
    
    /* JSON parsing variables */
    JSON_Value *root_val = NULL;
    JSON_Object *txpk_obj = NULL;
    JSON_Value *val = NULL; /* needed to detect the absence of some fields */
    const char *str; /* pointer to sub-strings in the JSON data */
    short x0, x1;
    
    /* auto-quit variable */
    uint32_t autoquit_cnt = 0; /* count the number of PULL_DATA sent since the latest PULL_ACK */
    
    /* Just In Time downlink */
    struct timeval current_unix_time;
    enum jit_error_e jit_result = JIT_ERROR_OK;
    enum jit_pkt_type_e downlink_type;

    /* set downstream socket RX timeout */
    i = setsockopt(sock_down, SOL_SOCKET, SO_RCVTIMEO, (void *)&pull_timeout, sizeof pull_timeout);
    if (i != 0) {
	    MSG_LOG(DEBUG_ERROR, "ERROR~ [down] setsockopt returned %s\n", strerror(errno));
	    exit(EXIT_FAILURE);
    }
    
    /* pre-fill the pull request buffer with fixed fields */
    buff_req[0] = PROTOCOL_VERSION;
    buff_req[3] = PKT_PULL_DATA;
    *(uint32_t *)(buff_req + 4) = net_mac_h;
    *(uint32_t *)(buff_req + 8) = net_mac_l;

    /* JIT queue initialization */
    jit_queue_init(&jit_queue); 

    while (!exit_sig && !quit_sig) {
	    
	/* auto-quit if the threshold is crossed */
	if ((autoquit_threshold > 0) && (autoquit_cnt >= autoquit_threshold)) {
		exit_sig = true;
		MSG_LOG(DEBUG_INFO, "INFO~ [down] the last %u PULL_DATA were not ACKed, exiting application\n", autoquit_threshold);
		break;
	}
	
	/* generate random token for request */
	token_h = (uint8_t)rand(); /* random token */
	token_l = (uint8_t)rand(); /* random token */
	buff_req[1] = token_h;
	buff_req[2] = token_l;

	/* send PULL request and record time */
	send(sock_down, (void *)buff_req, sizeof buff_req, 0);
	clock_gettime(CLOCK_MONOTONIC, &send_time);
        //MSG("INFOERROR [down] send pull_data, %ld\n", send_time.tv_nsec);
	pthread_mutex_lock(&mx_meas_dw);
	meas_dw_pull_sent += 1;
	pthread_mutex_unlock(&mx_meas_dw);
	req_ack = false;
	autoquit_cnt++;
	
	/* listen to packets and process them until a new PULL request must be sent */
	recv_time = send_time;
	while ((int)difftimespec(recv_time, send_time) < keepalive_time) {
		
	    /* try to receive a datagram */
	    msg_len = recv(sock_down, (void *)buff_down, (sizeof buff_down)-1, 0);
	    clock_gettime(CLOCK_MONOTONIC, &recv_time);
	    
	    /* if no network message was received, got back to listening sock_down socket */
	    if (msg_len == -1) {
		    //MSG("WARNING: [down] recv returned %s\n", strerror(errno)); /* too verbose */
		    continue;
	    }
	    
	    /* if the datagram does not respect protocol, just ignore it */
	    if ((msg_len < 4) || (buff_down[0] != PROTOCOL_VERSION) || ((buff_down[3] != PKT_PULL_RESP) && (buff_down[3] != PKT_PULL_ACK))) {
		    MSG_LOG(DEBUG_WARNING, "WARNING: [down] ignoring invalid packet\n");
		    continue;
	    }
	    
	    /* if the datagram is an ACK, check token */
	    if (buff_down[3] == PKT_PULL_ACK) {
		    if ((buff_down[1] == token_h) && (buff_down[2] == token_l)) {
			    if (req_ack) {
				    MSG_LOG(DEBUG_INFO, "INFO~ [down] duplicate ACK received :)\n");
			    } else { /* if that packet was not already acknowledged */
				    req_ack = true;
				    autoquit_cnt = 0;
				    pthread_mutex_lock(&mx_meas_dw);
				    meas_dw_ack_rcv += 1;
				    pthread_mutex_unlock(&mx_meas_dw);
				    //MSG("INFO~ [down] PULL_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
			    }
		    } else { /* out-of-sync token */
			    MSG_LOG(DEBUG_INFO, "INFO~ [down] received out-of-sync ACK\n");
		    }
		    continue;
	    }
	    
	    /* the datagram is a PULL_RESP */
	    buff_down[msg_len] = 0; /* add string terminator, just to be safe */
	    //printf("INFO~ [down] PULL_RESP received :)\n"); /* very verbose */
	    MSG_LOG(DEBUG_PKT_FWD, "\nRXTX~ (TXPK): [down] %s\n", (char *)(buff_down + 4)); /* DEBUG: display JSON payload */
	    
	    /* initialize TX struct and try to parse JSON */
	    memset(&txpkt, 0, sizeof(txpkt));
	    root_val = json_parse_string_with_comments((const char *)(buff_down + 4)); /* JSON offset */
	    if (root_val == NULL) {
		    MSG_LOG(DEBUG_WARNING, "WARNING: [down] invalid JSON, TX aborted\n");
		    continue;
	    }
	    
	    /* look for JSON sub-object 'txpk' */
	    txpk_obj = json_object_get_object(json_value_get_object(root_val), "txpk");
	    if (txpk_obj == NULL) {
		    MSG_LOG(DEBUG_WARNING, "WARNING: [down] no \"txpk\" object in JSON, TX aborted\n");
		    json_value_free(root_val);
		    continue;
	    }
	    

            /* TX procedure: send on timestamp value */
            val = json_object_get_value(txpk_obj,"tmst");
            if (val != NULL) {
                sent_immediate = false;
                txpkt.count_us = (uint32_t)json_value_get_number(val);
                downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_A;
            }

            /* Parse "No CRC" flag (optional field) */
            val = json_object_get_value(txpk_obj,"ncrc");
            if (val != NULL) {
                txpkt.no_crc = (bool)json_value_get_boolean(val);
            }

            /* parse target frequency (mandatory) */
            val = json_object_get_value(txpk_obj,"freq");
            if (val == NULL) {
                MSG_LOG(DEBUG_WARNING, "WARNING~ [down] no mandatory \"txpk.freq\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.freq_hz = (uint32_t)((double)(1.0e6) * json_value_get_number(val));

            /* Parse Lora spreading-factor and modulation bandwidth (mandatory) */
            str = json_object_get_string(txpk_obj, "datr");
            if (str == NULL) {
                MSG_LOG(DEBUG_WARNING, "WARNING~ [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            i = sscanf(str, "SF%2hdBW%3hd", &x0, &x1);
            if (i != 2) {
                MSG_LOG(DEBUG_WARNING, "WARNING~ [down] format error in \"txpk.datr\", TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            switch (x0) {
                case  7: txpkt.datarate = DR_LORA_SF7;  break;
                case  8: txpkt.datarate = DR_LORA_SF8;  break;
                case  9: txpkt.datarate = DR_LORA_SF9;  break;
                case 10: txpkt.datarate = DR_LORA_SF10; break;
                case 11: txpkt.datarate = DR_LORA_SF11; break;
                case 12: txpkt.datarate = DR_LORA_SF12; break;
                default:
                    MSG_LOG(DEBUG_WARNING, "WARNING~ [down] format error in \"txpk.datr\", invalid SF, TX aborted\n");
                    json_value_free(root_val);
                    continue;
            }
            switch (x1) {
                case 125: txpkt.bandwidth = BW_125KHZ; break;
                case 250: txpkt.bandwidth = BW_250KHZ; break;
                case 500: txpkt.bandwidth = BW_500KHZ; break;
                default:
                    MSG_LOG(DEBUG_WARNING, "WARNING~ [down] format error in \"txpk.datr\", invalid BW, TX aborted\n");
                    json_value_free(root_val);
                    continue;
            }

            /* Parse ECC coding rate (optional field) */
            str = json_object_get_string(txpk_obj, "codr");
            if (str == NULL) {
                MSG_LOG(DEBUG_WARNING, "WARNING~ [down] no mandatory \"txpk.codr\" object in json, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            if      (strcmp(str, "4/5") == 0) txpkt.coderate = CR_LORA_4_5;
            else if (strcmp(str, "4/6") == 0) txpkt.coderate = CR_LORA_4_6;
            else if (strcmp(str, "2/3") == 0) txpkt.coderate = CR_LORA_4_6;
            else if (strcmp(str, "4/7") == 0) txpkt.coderate = CR_LORA_4_7;
            else if (strcmp(str, "4/8") == 0) txpkt.coderate = CR_LORA_4_8;
            else if (strcmp(str, "1/2") == 0) txpkt.coderate = CR_LORA_4_8;
            else {
                MSG_LOG(DEBUG_WARNING, "WARNING~ [down] format error in \"txpk.codr\", TX aborted\n");
                json_value_free(root_val);
                continue;
            }

            /* Parse signal polarity switch (optional field) */
            val = json_object_get_value(txpk_obj,"ipol");
            if (val != NULL) {
                txpkt.invert_pol = (bool)json_value_get_boolean(val);
            }

            /* parse Lora preamble length (optional field, optimum min value enforced) */
            val = json_object_get_value(txpk_obj,"prea");
            if (val != NULL) {
                i = (int)json_value_get_number(val);
                if (i >= MIN_LORA_PREAMB) {
                    txpkt.preamble = (uint16_t)i;
                } else {
                    txpkt.preamble = (uint16_t)MIN_LORA_PREAMB;
                }
            } else {
                txpkt.preamble = (uint16_t)STD_LORA_PREAMB;
            }

	    /* Parse payload length (mandatory) */
	    val = json_object_get_value(txpk_obj,"size");
	    if (val == NULL) {
		    MSG_LOG(DEBUG_WARNING, "WARNING~ [down] no mandatory \"txpk.size\" object in JSON, TX aborted\n");
		    json_value_free(root_val);
		    continue;
	    }
	    txpkt.size = (uint16_t)json_value_get_number(val);
	    
	    /* Parse payload data (mandatory) */
	    str = json_object_get_string(txpk_obj, "data"); if (str == NULL) {
		    MSG_LOG(DEBUG_WARNING, "WARNING~ [down] no mandatory \"txpk.data\" object in JSON, TX aborted\n");
		    json_value_free(root_val);
		    continue;
	    }

	    i = b64_to_bin(str, strlen(str), txpkt.payload, sizeof(txpkt.payload));
	    if (i != txpkt.size) {
		MSG_LOG(DEBUG_WARNING, "WARNING~ [down] mismatch between .size and .data size once converter to binary\n");
	    }

            //MSG_LOG(DEBUG_INFO, "INFO~ [down]receive txpk payload %d byte)\n", txpkt.size);

	    /* free the JSON parse tree from memory */
	    json_value_free(root_val);
	    
	    /* select TX mode */
	    if (sent_immediate) 
		    txpkt.tx_mode = IMMEDIATE;
	    else
		    txpkt.tx_mode = TIMESTAMPED;

            /*Maybe need send immediate*/
            txlora(txdev, &txpkt);
	    
	    /* record measurement data */
	    pthread_mutex_lock(&mx_meas_dw);
	    meas_dw_dgram_rcv += 1; /* count only datagrams with no JSON errors */
	    meas_dw_network_byte += msg_len; /* meas_dw_network_byte */
	    meas_dw_payload_byte += txpkt.size;
            meas_nb_tx_ok += 1;
            pthread_mutex_unlock(&mx_meas_dw);

            /* check TX parameter before trying to queue packet */                                      
            gettimeofday(&current_unix_time, NULL);
            jit_result = jit_enqueue(&jit_queue, &current_unix_time, &txpkt, downlink_type);
            if (jit_result != JIT_ERROR_OK) {
                MSG_LOG(DEBUG_ERROR, "ERROR~ Packet REJECTED (jit error=%d)\n", jit_result);
            }
            pthread_mutex_lock(&mx_meas_dw);
            meas_nb_tx_requested += 1;
            pthread_mutex_unlock(&mx_meas_dw);

            /* Send acknoledge datagram to server */
            send_tx_ack(buff_down[1], buff_down[2], jit_result);
	}
    }
    MSG_LOG(DEBUG_INFO, "\nINFO~ End of downstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 4: CHECKING PACKETS TO BE SENT FROM JIT QUEUE AND SEND THEM --- */

void thread_jit(void) {
    struct pkt_tx_s pkt;
    int pkt_index = -1;
    struct timeval current_unix_time;
    enum jit_error_e jit_result;
    enum jit_pkt_type_e pkt_type;

    while (!exit_sig && !quit_sig) {
        wait_ms(10);
        /* transfer data and metadata to the concentrator, and schedule TX */
        gettimeofday(&current_unix_time, NULL);
        jit_result = jit_peek(&jit_queue, &current_unix_time, &pkt_index);
        if (jit_result == JIT_ERROR_OK) {
            if (pkt_index > -1) {
                jit_result = jit_dequeue(&jit_queue, pkt_index, &pkt, &pkt_type);
                if (jit_result == JIT_ERROR_OK) {
                    txlora(txdev, &pkt);
                    pthread_mutex_lock(&mx_meas_dw);
                    meas_nb_tx_ok += 1;
                    pthread_mutex_unlock(&mx_meas_dw);
                    MSG_LOG(DEBUG_PKT_FWD, "INFO~ Donwlink done: count_us=%u\n", pkt.count_us);
                } else {
                    MSG_LOG(DEBUG_ERROR, "ERROR~ jit_dequeue failed with %d\n", jit_result);
                }
            }
        } else if (jit_result == JIT_ERROR_EMPTY) {
            /* Do nothing, it can happen */
        } else {
            MSG_LOG(DEBUG_ERROR, "ERROR~ jit_peek failed with %d\n", jit_result);
        }
    }
}

/* --- EOF ------------------------------------------------------------------ */
