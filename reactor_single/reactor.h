/*************************************************************************
	> File Name: reactor.h
	> Author: JianWei
	> Mail: wj_clear@163.com 
	> Created Time: 2022年11月07日 星期一 16时45分27秒
 ************************************************************************/
#define ITEM_BUFFER_LENGTH			1024
#define ACCEPT_KEY_LENGTH			64
#define SOCK_LISTEN_ADD_NUM			128
#define MAX_EPOLL_EVENTS			1024
#define MISC_GUID					"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define ACCEPT_KEY_LENGTH			64

enum {      
    WS_HANDSHARK = 0,
    WS_TRANMISSION = 1,
    WS_END = 2,
    WS_COUNT
};      

                           
struct ws_ophdr {

    unsigned char opcode:4,
                rsv3:1,
                rsv2:1,
                rsv1:1,
                fin:1;
    
    unsigned char pl_len:7,
                  mask:1;

};


typedef int misc_event_callback(int fd, int events, void *arg);

typedef struct {
	int fd;		///< event fd
	int events;	///< event status
	void *arg;	///< extensible mem

	misc_event_callback* call_back_func;		///< function ptr
	int status;
	
	uint8_t  rbuffer[ITEM_BUFFER_LENGTH];	 ///<	get value
	uint32_t rlength;
	uint8_t  wbuffer[ITEM_BUFFER_LENGTH];    ///<	post value
	uint32_t wlength;
	
	char	sec_accept[ACCEPT_KEY_LENGTH];
	int		wsstatus;

}reactor_item_t;


///<event  item bucket  list
typedef struct eventblock {
    
    struct eventblock	*next;			///< next node
    reactor_item_t		*item_bucket;	///< item 
}event_list_node_t;


///< reactor 
typedef struct{
	int epoll_fd;
	int block_count;

	event_list_node_t *list_header;
}reactor_t;

