/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// net.h -- quake's interface to the networking layer

#define	PORT_ANY	-1

#if defined(NACL) || defined(FTE_TARGET_WEB)
#define HAVE_WEBSOCKCL
#endif

//FIXME: should split this into loopback/dgram/stream/irc
//with the ipv4/v6/x as a separate parameter
typedef enum {NA_INVALID, NA_LOOPBACK, NA_IP, NA_IPV6, NA_IPX, NA_BROADCAST_IP, NA_BROADCAST_IP6, NA_BROADCAST_IPX, NA_TCP, NA_TCPV6, NA_TLSV4, NA_TLSV6, NA_IRC, NA_WEBSOCKET, NA_NATPMP} netadrtype_t;

typedef enum {NS_CLIENT, NS_SERVER} netsrc_t;

typedef enum {NQP_ERROR, NQP_DATAGRAM, NQP_RELIABLE} nqprot_t;

typedef struct
{
	netadrtype_t	type;

	union {
		qbyte	ip[4];
		qbyte	ip6[16];
		qbyte	ipx[10];
#ifdef IRCCONNECT
		struct {
			char host[32];
			char user[32];
			char channel[12];
		} irc;
#endif
#ifdef HAVE_WEBSOCKCL
		char websocketurl[64];
#endif
	} address;

	unsigned short	port;
	unsigned short	connum;	//which quake connection/socket the address is talking about. 1-based. 0 is unspecified.
	unsigned int scopeid;	//ipv6 interface id thing.
} netadr_t;

struct sockaddr_qstorage
{
	short dontusesa_family;
	unsigned char dontusesa_pad[6];
	qint64_t sa_align;
	unsigned char sa_pad2[112];
};


extern	netadr_t	net_local_cl_ipadr;
extern	netadr_t	net_from;		// address of who sent the packet
extern	sizebuf_t	net_message;
//#define	MAX_UDP_PACKET	(MAX_MSGLEN*2)	// one more than msg + header
#define	MAX_UDP_PACKET	8192	// one more than msg + header
extern	FTE_ALIGN(4) qbyte		net_message_buffer[MAX_OVERALLMSGLEN];

typedef enum
{
	NETERR_SENT			= 0,	//all is well
	NETERR_NOROUTE		= 1,	//destination isn't valid for this socket/etc. try a different one if possible
	NETERR_DISCONNECTED = 2,	//socket can no longer send anything
	NETERR_MTU			= 3,	//packet wasn't sent due to MTU
	NETERR_CLOGGED		= 4		//socket is suffering from conjestion
} neterr_t;

extern	cvar_t	hostname;

int TCP_OpenStream (netadr_t *remoteaddr);	//makes things easier

struct ftenet_connections_s;
void		NET_Init (void);
void		NET_Tick (void);
void		SVNET_RegisterCvars(void);
void		NET_InitClient (qboolean loopbackonly);
void		NET_InitServer (void);
qboolean	NET_WasSpecialPacket(netsrc_t netsrc);
void		NET_CloseServer (void);
void		UDP_CloseSocket (int socket);
void		NET_Shutdown (void);
qboolean	NET_GetRates(struct ftenet_connections_s *collection, float *pi, float *po, float *bi, float *bo);
qboolean	NET_UpdateRates(struct ftenet_connections_s *collection, qboolean inbound, size_t size);	//for demos to not be weird
int			NET_GetPacket (netsrc_t netsrc, int firstsock);
neterr_t	NET_SendPacket (netsrc_t socket, int length, const void *data, netadr_t *to);
int			NET_LocalAddressForRemote(struct ftenet_connections_s *collection, netadr_t *remote, netadr_t *local, int idx);
void		NET_PrintAddresses(struct ftenet_connections_s *collection);
qboolean	NET_AddressSmellsFunny(netadr_t *a);
qboolean	NET_EnsureRoute(struct ftenet_connections_s *collection, char *routename, char *host, qboolean islisten);

enum addressscope_e
{
	ASCOPE_PROCESS=0,
	ASCOPE_HOST=1,
	ASCOPE_LAN=2,
	ASCOPE_NET=3
};
enum addressscope_e NET_ClassifyAddress(netadr_t *adr, char **outdesc);

qboolean	NET_CompareAdr (netadr_t *a, netadr_t *b);
qboolean	NET_CompareBaseAdr (netadr_t *a, netadr_t *b);
void		NET_AdrToStringResolve (netadr_t *adr, void (*resolved)(void *ctx, void *data, size_t a, size_t b), void *ctx, size_t a, size_t b);
char		*NET_AdrToString (char *s, int len, netadr_t *a);
char		*NET_BaseAdrToString (char *s, int len, netadr_t *a);
size_t		NET_StringToSockaddr2 (const char *s, int defaultport, struct sockaddr_qstorage *sadr, int *addrfamily, int *addrsize, size_t addrcount);
#define NET_StringToSockaddr(s,p,a,f,z) (NET_StringToSockaddr2(s,p,a,f,z,1)>0)
size_t		NET_StringToAdr2 (const char *s, int defaultport, netadr_t *a, size_t addrcount);
#define NET_StringToAdr(s,p,a) NET_StringToAdr2(s,p,a,1)
qboolean	NET_PortToAdr (int adrfamily, const char *s, netadr_t *a);
qboolean NET_IsClientLegal(netadr_t *adr);

qboolean	NET_IsLoopBackAddress (netadr_t *adr);

qboolean NET_StringToAdrMasked (const char *s, netadr_t *a, netadr_t *amask);
char	*NET_AdrToStringMasked (char *s, int len, netadr_t *a, netadr_t *amask);
void NET_IntegerToMask (netadr_t *a, netadr_t *amask, int bits);
qboolean NET_CompareAdrMasked(netadr_t *a, netadr_t *b, netadr_t *mask);

qboolean FTENET_AddToCollection(struct ftenet_connections_s *col, const char *name, const char *address, netadrtype_t addrtype, qboolean islisten);

//============================================================================

#define	OLD_AVG		0.99		// total = oldtotal*OLD_AVG + new*(1-OLD_AVG)

#define	MAX_LATENT	32
#define MAX_ADR_SIZE	64

typedef struct
{
	qboolean	fatal_error;

#ifdef NQPROT
	int	isnqprotocol;
	qboolean	nqreliable_allowed;	//says the peer has acked the last reliable (or timed out and needs resending).
	float		nqreliable_resendtime;//force nqreliable_allowed, thereby forcing a resend of anything n
	qbyte		nqunreliableonly;	//nq can't cope with certain reliables some times. if 2, we have a reliable that result in a block (that should be sent). if 1, we are blocking. if 0, we can send reliables freely.
#endif
	struct netprim_s netprim;
	int			fragmentsize;
	int			dupe;

	float		last_received;		// for timeouts

// the statistics are cleared at each client begin, because
// the server connecting process gives a bogus picture of the data
	float		frame_latency;		// rolling average
	float		frame_rate;

	int			drop_count;			// dropped packets, cleared each level
	int			good_count;			// cleared each level

	int			bytesin;
	int			bytesout;

	netadr_t	remote_address;
	netsrc_t	sock;
	int			qport;
	int			qportsize;

// bandwidth estimator
	double		cleartime;			// if realtime > nc->cleartime, free to go
//	double		rate;				// seconds / qbyte

// sequencing variables
	int			incoming_unreliable;	//dictated by the other end.
	int			incoming_sequence;
	int			incoming_acknowledged;
	int			incoming_reliable_acknowledged;	// single bit

	int			incoming_reliable_sequence;		// single bit, maintained local

	int			outgoing_unreliable;
	int			outgoing_sequence;
	int			reliable_sequence;			// single bit
	int			last_reliable_sequence;		// sequence number of last send

// reliable staging and holding areas
	sizebuf_t	message;		// writing buffer to send to server
	qbyte		message_buf[MAX_OVERALLMSGLEN];

	//nq has message truncation.
	int			reliable_length;
	int			reliable_start;
	qbyte		reliable_buf[MAX_OVERALLMSGLEN];	// unacked reliable message

// time and size data to calculate bandwidth
	int			outgoing_size[MAX_LATENT];
	double		outgoing_time[MAX_LATENT];
	struct huffman_s	*compresstable;

	//nq servers must recieve truncated packets.
	int in_fragment_length;
	char in_fragment_buf[MAX_OVERALLMSGLEN];
	int in_fragment_start;
} netchan_t;

extern	int	net_drop;		// packets dropped before this one

void Net_Master_Init(void);

void Netchan_Init (void);
int Netchan_Transmit (netchan_t *chan, int length, qbyte *data, int rate);
void Netchan_OutOfBand (netsrc_t sock, netadr_t *adr, int length, qbyte *data);
void VARGS Netchan_OutOfBandPrint (netsrc_t sock, netadr_t *adr, char *format, ...) LIKEPRINTF(3);
void VARGS Netchan_OutOfBandTPrintf (netsrc_t sock, netadr_t *adr, int language, translation_t text, ...);
qboolean Netchan_Process (netchan_t *chan);
void Netchan_Setup (netsrc_t sock, netchan_t *chan, netadr_t *adr, int qport);
unsigned int Net_PextMask(int maskset, qboolean fornq);
extern cvar_t net_mtu;

qboolean Netchan_CanPacket (netchan_t *chan, int rate);
void Netchan_Block (netchan_t *chan, int bytes, int rate);
qboolean Netchan_CanReliable (netchan_t *chan, int rate);
#ifdef NQPROT
nqprot_t NQNetChan_Process(netchan_t *chan);
#endif

#ifdef HUFFNETWORK
#define HUFFCRC_QUAKE3 0x286f2e8d

typedef struct huffman_s huffman_t;
int Huff_PreferedCompressionCRC (void);
void Huff_EncryptPacket(sizebuf_t *msg, int offset);
void Huff_DecryptPacket(sizebuf_t *msg, int offset);
huffman_t *Huff_CompressionCRC(int crc);
void Huff_CompressPacket(huffman_t *huff, sizebuf_t *msg, int offset);
void Huff_DecompressPacket(huffman_t *huff, sizebuf_t *msg, int offset);
int Huff_GetByte(qbyte *buffer, int *count);
void Huff_EmitByte(int ch, qbyte *buffer, int *count);
#endif

#ifdef NQPROT
//taken from nq's net.h
//refer to that for usage info. :)

#define NETFLAG_LENGTH_MASK	0x0000ffff
#define NETFLAG_DATA		0x00010000
#define NETFLAG_ACK			0x00020000
#define NETFLAG_NAK			0x00040000
#define NETFLAG_EOM			0x00080000
#define NETFLAG_UNRELIABLE	0x00100000
#define NETFLAG_CTL			0x80000000

#define NQ_NETCHAN_GAMENAME	"QUAKE"
#define NQ_NETCHAN_VERSION	3


#define CCREQ_CONNECT		0x01
#define CCREQ_SERVER_INFO	0x02
#define CCREQ_PLAYER_INFO	0x03
#define CCREQ_RULE_INFO		0x04

#define CCREP_ACCEPT		0x81
#define CCREP_REJECT		0x82
#define CCREP_SERVER_INFO	0x83
#define CCREP_PLAYER_INFO	0x84
#define CCREP_RULE_INFO		0x85

//server->client protocol info
#define PROTOCOL_VERSION_NQ		15
#define PROTOCOL_VERSION_H2		19
#define PROTOCOL_VERSION_NEHD	250
#define PROTOCOL_VERSION_FITZ	666
#define PROTOCOL_VERSION_RMQ	999
#define PROTOCOL_VERSION_DP5	3502
#define PROTOCOL_VERSION_DP6	3503
#define PROTOCOL_VERSION_DP7	3504
#define PROTOCOL_VERSION_BJP1	10000
#define PROTOCOL_VERSION_BJP2	10001
#define PROTOCOL_VERSION_BJP3	10002

/*RMQ protocol flags*/
#define RMQFL_SHORTANGLE	(1 << 1)
#define RMQFL_FLOATANGLE	(1 << 2)
#define RMQFL_24BITCOORD	(1 << 3)
#define RMQFL_FLOATCOORD	(1 << 4)
#define RMQFL_EDICTSCALE	(1 << 5)
#define RMQFL_ALPHASANITY	(1 << 6)
#define RMQFL_INT32COORD	(1 << 7)
#define RMQFL_MOREFLAGS		(1 << 31)

#endif

int UDP_OpenSocket (int port, qboolean bcast);
int UDP6_OpenSocket (int port, qboolean bcast);
int IPX_OpenSocket (int port, qboolean bcast);
int NetadrToSockadr (netadr_t *a, struct sockaddr_qstorage *s);
void SockadrToNetadr (struct sockaddr_qstorage *s, netadr_t *a);
qboolean NET_Sleep(float seconds, qboolean stdinissocket);
