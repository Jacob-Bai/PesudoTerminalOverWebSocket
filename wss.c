#include <libwebsockets.h>
// #include <private-lib-core.h>
#include <string.h>
#include <stdio.h>
#include <pty.h>
#include <signal.h>
#include <time.h>

char* version="2.1";
// shell to run
char *shell = "/bin/sh";
// verbose level
uint8_t verbose = 0;
// port
int ws_port = 5000;
// maximum connection
uint8_t max_connection = 3;
// timeout for connectons
int ws_timeout = 300;
// timeout to exit program
int proc_timeout = 60;

// max + 1: always have 1 socket free for connect
#define PTY_MAX_CONNECTION  (max_connection + 1)
#define MAX_MSG_SIZE        1024
// 5 mins for activated connections
#define PTY_TIMEOUT         ws_timeout 
// if no connection made within 60 seconds after program started, we exit this 
#define PROC_TIMEOUT        proc_timeout 


typedef enum PTY_CONN_STATUS {
    AVAILABLE,
    CONNECTED,
    SERVING,
    TIMEOUT,
    DISCONNECTED,
} pty_status;

typedef struct wsPTY {
    struct lws *wsi;
    char pty_name[32];
    int pty_master;
    pthread_t pty_thread;
    pty_status status;
    time_t last_contact;
    int child_pid;
} WS_PTY;

typedef struct msgBuf {
    char buff[MAX_MSG_SIZE];
    int size;
} MSG_BUF;

uint8_t* pty_id;
WS_PTY* ptys;
MSG_BUF* msgs;

pthread_attr_t attr;
pthread_t pty_manager;
// pid when forking 
int pty_pid;
// fthe start time of this program
time_t prog_start_time;

// return current epoch seconds
time_t get_second_now (void) {
    struct timeval  tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

// static void signal_handler(int sig, siginfo_t* sinfo, void* context) {
//     if (sig == SIGCHLD) {
//         // fprintf(stderr, "INFO: execl('%s') quit with status %d\n", cmd, sinfo->si_status);
//         printf("shell stopped\n");
//     }
// }

// listening from the shell and writing to websocket
// exit if connection timeout or disconnected
void *pty_task(void *args) { 
    uint8_t this_conn = *(uint8_t*)args;
    int cnt;
    // if(verbose) printf("ready to serve connection %d\n", this_conn);
    while (ptys[this_conn].status == SERVING) {
        memset(msgs[this_conn].buff, 0, MAX_MSG_SIZE);
        cnt = read(ptys[this_conn].pty_master, msgs[this_conn].buff, MAX_MSG_SIZE);
        if (cnt > 0 && ptys[this_conn].status == SERVING) {
            if(verbose == 2) {
                printf("message from shell %d :", this_conn);
                fprintf(stdout, "%s\n", msgs[this_conn].buff);
                fflush(stdout);
            }
            ptys[this_conn].last_contact = get_second_now();
            msgs[this_conn].size = cnt;
            lws_callback_on_writable( ptys[this_conn].wsi );
            while (msgs[this_conn].size)
                usleep(100);
        }
    }
    
    if (ptys[this_conn].status == TIMEOUT)
        // close this connection
        lws_close_free_wsi(ptys[this_conn].wsi, LWS_CLOSE_STATUS_NORMAL, NULL);
    if(verbose) printf("stop serving connection: %d\n", this_conn);
    return NULL;
}

void *pty_manager_task (void *args) {
    uint8_t free_socket;
    time_t sec_now;
    uint8_t inact_conn;
    while (1) {
        free_socket = 0;
        sec_now = get_second_now();
        inact_conn = 0;
        for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++) {
            switch(ptys[i].status) {
                case AVAILABLE:
                    // we have socket free for new connection
                    free_socket++;
                    break;

                case CONNECTED:
                    // TO serving
                    pty_pid = forkpty(&ptys[i].pty_master, ptys[i].pty_name, NULL, NULL);
                    if (pty_pid < 0) {
                        fprintf(stderr, "forkpty %d failed\n", i);
                        exit(1);
                    }
                    if (pty_pid == 0) {
                        // child process
                        execl(shell, shell, NULL);
                        return NULL;
                    } else {
                        ptys[i].child_pid = pty_pid;
                        // update last contact
                        ptys[i].last_contact = sec_now;
                        // create thread to serve this connection
                        ptys[i].status = SERVING;
                        if(pthread_create(&ptys[i].pty_thread, &attr, pty_task, (void*)&pty_id[i]))
                            exit(1);
                        if(verbose) printf("started serving connection: %d\n", i);
                    }
                    break;

                case SERVING:
                    // check if timeout
                    if (sec_now - ptys[i].last_contact > PTY_TIMEOUT) {
                        if(verbose) printf("connection %d timeout\n", i);
                        // set this connection to timeout
                        ptys[i].status = TIMEOUT;
                        // read will block the process, so send something to kick it off
                        write(ptys[i].pty_master, " ", 1); 
                    }   
                    if (!free_socket && ptys[i].last_contact < ptys[inact_conn].last_contact)
                        // if there is no free socket, we need to find the most inactive one and close it
                        inact_conn = i;
                    break;

                case TIMEOUT:
                    // this socket will be free, no need to clean up
                    free_socket = 1;
                    break;

                case DISCONNECTED:
                    // kill child (shell)
                    if (ptys[i].child_pid) {
                        kill(ptys[i].child_pid, SIGKILL);
                        close(ptys[i].pty_master);
                    }
                        
                    // reset buffer
                    memset(&ptys[i], 0, sizeof(WS_PTY));
                    if (verbose) printf("cleaned up connection: %d\n", i);
                    // this socket will be free, no need to clean up
                    free_socket = 1;
                    break;

                default:
                    break;
            }
        }
        if (free_socket == 0){
            // clean up one
            if(verbose) printf("no free sockets, disconnecting connection: %d\n", inact_conn);
            ptys[inact_conn].status = TIMEOUT;
            // read will block the process, so send something to kick it off
            // write(ptys[inact_conn].pty_master, " ", 1); 
        } else if ( PROC_TIMEOUT && sec_now - prog_start_time > PROC_TIMEOUT ) {
            // either there is no connection made after program started
            // or all connection has been disconnected
            if(verbose) printf("no connection listed, exit\n");
            exit(0);
        }
            
        usleep(100);
    }

    return NULL;
}

static int lws_callback( struct lws *wsi, enum lws_callback_reasons reason, void *connection, void *in, size_t len )
{
	switch( reason )
	{
        case LWS_CALLBACK_ESTABLISHED:
            for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++) {
                if (ptys[i].status == AVAILABLE) {
                    if(verbose) printf("connection %d connected\n", i);
                    ptys[i].wsi = wsi;
                    ptys[i].status = CONNECTED;
                    break;
                }
                if (i == PTY_MAX_CONNECTION - 1)
                    return -1;
            }
            break;
		case LWS_CALLBACK_RECEIVE:
            for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++) {
                if (ptys[i].wsi == wsi) {
                    if(verbose == 2) printf("message from connection: %d len: %ld\n", i, (long) len);
                    //fprintf(stdout, "message from connection:%d, in:%.*s, len:%ld\n", i, (int) len, (char *) in, (long) len);
                    write(ptys[i].pty_master, in, len);
                    break;
                }
            }
			break;
		case LWS_CALLBACK_SERVER_WRITEABLE:
            for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++) {
                if (ptys[i].wsi == wsi && ptys[i].status == SERVING) { // check if it's serving, since calling lws_close_free_wsi() will go there, no idea why
                    if(verbose) printf("message to connection: %d len: %d\n", i, msgs[i].size);
                    lws_write(wsi, msgs[i].buff, msgs[i].size, LWS_WRITE_TEXT);
                    msgs[i].size = 0;
                    break;
                }
            }
			break;
        case LWS_CALLBACK_CLOSED:
            for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++) {
                if (ptys[i].wsi == wsi) {
                    if(verbose) printf("connection %d disconnected\n", i);
                    ptys[i].status = DISCONNECTED;
                    break;
                }
            }
            break;
		default:
			break;
	}

	return 0;
}

enum protocols
{
	PROTOCOL_HTTP = 0,
	PROTOCOL_EXAMPLE,
	PROTOCOL_COUNT
};

static struct lws_protocols protocols[] =
{
    // {
	// 	"http-only",   /* name */
	// 	callback_http, /* callback */
	// 	0,             /* No per session data. */
	// 	0,             /* max frame size / rx buffer */
	// },
	{
		"", // protocol
		lws_callback,
		0,
		0,
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};

void printhelp(char *app) {
    fprintf(stderr, "WebTerminal %s\n", version);
    fprintf(stderr, " -s <filename>     which shell, default: '/bin/sh'\n");
    fprintf(stderr, " -p <num>          the port this server is listening to; default is 5000\n");
    fprintf(stderr, " -t <num>          timeout for inactive connection; default is 300(seconds)\n");
    fprintf(stderr, " -e <num>          timeout for exit this process if no connection; default is 60(seconds), 0 for disable\n");
    fprintf(stderr, " -m <num>          maximum connections; default is 3, max num is 8\n");
    fprintf(stderr, " -v <num>          level of verbose; default is 0; 1 is connection level; 2 is message level\n");
    fprintf(stderr, " -h                show this help\n");
}

int main( int argc, char *argv[] )
{
    int c;
    while ((c = getopt(argc, argv, "s:p:t:e:m:v:h:?")) != -1) {
        switch (c) {
            case 's':
                shell = optarg;
                break;
            case 'p':
                ws_port = atoi(optarg);
                break;
            case 't':
                ws_timeout = atoi(optarg);
                break;
            case 'e':
                proc_timeout = atoi(optarg);
                break;
            case 'm':
                max_connection = atoi(optarg);
                break;
            case 'v':
                verbose = atoi(optarg);
                break;
            case 'h':
            case '?':
            default:
                printhelp(argv[0]);
                exit(1);
        }
    }

    printf("webTerminal %s started on port %d, allowing max %d connections and %ds timeout\n", version, ws_port, max_connection, ws_timeout);
    printf("using shell %s, with verbose level %d\n", shell, verbose);

    if (verbose == 2)
        lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);
    else
        lws_set_log_level(LLL_ERR, NULL);

    pty_id = malloc(sizeof(uint8_t) * PTY_MAX_CONNECTION);
    for (uint8_t i = 0; i < PTY_MAX_CONNECTION; i++)
        pty_id[i] = i;

    ptys = malloc(sizeof(WS_PTY) * PTY_MAX_CONNECTION);
    memset(ptys, 0, sizeof(WS_PTY) * PTY_MAX_CONNECTION);

    msgs = malloc(sizeof(MSG_BUF) * PTY_MAX_CONNECTION);
    memset(msgs, 0, sizeof(MSG_BUF) * PTY_MAX_CONNECTION);
    
    pthread_attr_init(&attr);
    // mem leak without detached attribute 
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    prog_start_time = get_second_now();
    pthread_create(&pty_manager, NULL, pty_manager_task, NULL);

    struct lws_context_creation_info info;
    memset( &info, 0, sizeof(info) );

    info.port = ws_port;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    // info.ssl_ca_filepath = "./key/ca-cert.pem";
    // info.ssl_cert_filepath = "./key/server-cert.pem";
    // info.ssl_private_key_filepath = "./key/server-key.pem";
    // info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    // info.options |= LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED;

    // struct sigaction sa;
    // sa.sa_handler = (void(*)(int))signal_handler;
    // sigemptyset(&sa.sa_mask);
    // sa.sa_flags = SA_RESTART;
    // sigaction(SIGCHLD, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);

    struct lws_context *context = lws_create_context( &info );

    while( 1 )
    {
        lws_service( context, /* timeout_ms = */ 5000 );
    }

    lws_context_destroy( context );

	return 0;
}