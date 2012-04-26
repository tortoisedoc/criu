#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/types.h>
#include <linux/net.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/sendfile.h>

#include "types.h"
#include "libnetlink.h"
#include "sockets.h"
#include "sk-queue.h"
#include "unix_diag.h"
#include "image.h"
#include "crtools.h"
#include "util.h"
#include "inet_diag.h"
#include "files.h"
#include "util-net.h"

static char buf[4096];

#ifndef NETLINK_SOCK_DIAG
#define NETLINK_SOCK_DIAG NETLINK_INET_DIAG
#endif

#ifndef SOCK_DIAG_BY_FAMILY
#define SOCK_DIAG_BY_FAMILY 20
#endif

#ifndef SOCKFS_MAGIC
#define SOCKFS_MAGIC	0x534F434B
#endif

struct unix_sk_desc {
	struct socket_desc	sd;
	unsigned int		type;
	unsigned int		state;
	unsigned int		peer_ino;
	unsigned int		rqlen;
	unsigned int		wqlen;
	unsigned int		namelen;
	char			*name;
	unsigned int		nr_icons;
	unsigned int		*icons;
	struct list_head	list;
};

static LIST_HEAD(unix_sockets);

struct unix_sk_listen_icon {
	unsigned int			peer_ino;
	struct unix_sk_desc		*sk_desc;
	struct unix_sk_listen_icon	*next;
};

#define SK_HASH_SIZE		32

static struct socket_desc *sockets[SK_HASH_SIZE];

static struct socket_desc *lookup_socket(int ino)
{
	struct socket_desc *sd;

	for (sd = sockets[ino % SK_HASH_SIZE]; sd; sd = sd->next)
		if (sd->ino == ino)
			return sd;
	return NULL;
}

static struct unix_sk_listen_icon *unix_listen_icons[SK_HASH_SIZE];

static struct unix_sk_listen_icon *lookup_unix_listen_icons(int peer_ino)
{
	struct unix_sk_listen_icon *ic;

	for (ic = unix_listen_icons[peer_ino % SK_HASH_SIZE];
			ic; ic = ic->next)
		if (ic->peer_ino == peer_ino)
			return ic;
	return NULL;
}

int sk_collect_one(int ino, int family, struct socket_desc *d)
{
	struct socket_desc **chain;

	d->ino		= ino;
	d->family	= family;

	chain = &sockets[ino % SK_HASH_SIZE];
	d->next = *chain;
	*chain = d;

	return 0;
}

static void show_one_unix(char *act, const struct unix_sk_desc *sk)
{
	pr_debug("\t%s: ino 0x%8x family %4d type %4d state %2d name %s\n",
		act, sk->sd.ino, sk->sd.family, sk->type, sk->state, sk->name);

	if (sk->nr_icons) {
		int i;

		for (i = 0; i < sk->nr_icons; i++)
			pr_debug("\t\ticon: %4d\n", sk->icons[i]);
	}
}

static void show_one_unix_img(const char *act, const struct unix_sk_entry *e)
{
	pr_info("\t%s: id %u type %d state %d name %d bytes\n",
		act, e->id, e->type, e->state, e->namelen);
}

static int can_dump_unix_sk(const struct unix_sk_desc *sk)
{
	if (sk->type != SOCK_STREAM &&
	    sk->type != SOCK_DGRAM) {
		pr_err("Only stream/dgram sockets for now\n");
		return 0;
	}

	switch (sk->state) {
	case TCP_LISTEN:
		break;
	case TCP_ESTABLISHED:
		break;
	case TCP_CLOSE:
		if (sk->type != SOCK_DGRAM)
			return 0;
		break;
	default:
		pr_err("Unknown state %d\n", sk->state);
		return 0;
	}

	return 1;
}

static int dump_one_unix(const struct socket_desc *_sk, struct fd_parms *p,
		int lfd, const struct cr_fdset *cr_fdset)
{
	struct unix_sk_desc *sk = (struct unix_sk_desc *)_sk;
	struct fdinfo_entry fe;
	struct unix_sk_entry ue;

	if (!can_dump_unix_sk(sk))
		goto err;

	fe.fd = p->fd;
	fe.type = FDINFO_UNIXSK;
	fe.id = sk->sd.ino;
	fe.flags = p->fd_flags;

	if (write_img(fdset_fd(cr_fdset, CR_FD_FDINFO), &fe))
		goto err;

	if (sk->sd.already_dumped)
		return 0;

	ue.id		= sk->sd.ino;
	ue.type		= sk->type;
	ue.state	= sk->state;
	ue.namelen	= sk->namelen;
	ue.flags	= p->flags;
	ue.backlog	= sk->wqlen;
	ue.peer		= sk->peer_ino;
	ue.fown		= p->fown;
	ue.uflags	= 0;

	if (ue.peer) {
		struct unix_sk_desc *peer;

		peer = (struct unix_sk_desc *)lookup_socket(ue.peer);
		if (!peer) {
			pr_err("Unix socket %#x without peer %#x\n",
					ue.id, ue.peer);
			goto err;
		}

		/*
		 * Peer should have us as peer or have a name by which
		 * we can access one.
		 */
		if (peer->peer_ino != ue.id) {
			if (!peer->name) {
				pr_err("Unix socket %#x with unreachable peer %#x (%#x/%s)\n",
				       ue.id, ue.peer, peer->peer_ino, peer->name);
				goto err;
			}

			/*
			 * It can be external socket, so we defer dumping
			 * until all sockets the program owns are processed.
			 */
			if (!peer->sd.already_dumped)
				list_add_tail(&peer->list, &unix_sockets);
		}
	} else if (ue.state == TCP_ESTABLISHED) {
		const struct unix_sk_listen_icon *e;

		/*
		 * If this is in-flight connection we need to figure
		 * out where to connect it on restore. Thus, tune up peer
		 * id by searching an existing listening socket.
		 *
		 * Note the socket name will be found at restore stage,
		 * not now, just to reduce size of dump files.
		 */

		e = lookup_unix_listen_icons(ue.id);
		if (!e) {
			pr_err("Dangling in-flight connection %d\n", ue.id);
			goto err;
		}

		/* e->sk_desc is _never_ NULL */
		if (e->sk_desc->state != TCP_LISTEN) {
			pr_err("In-flight connection on "
				"non-listening socket %d\n", ue.id);
			goto err;
		}

		ue.peer = e->sk_desc->sd.ino;

		pr_debug("\t\tFixed inflight socket %#x peer %#x)\n",
				ue.id, ue.peer);
	}

	if (write_img(fdset_fd(glob_fdset, CR_FD_UNIXSK), &ue))
		goto err;
	if (write_img_buf(fdset_fd(glob_fdset, CR_FD_UNIXSK), sk->name, ue.namelen))
		goto err;

	if (sk->rqlen != 0 && !(sk->type == SOCK_STREAM &&
				sk->state == TCP_LISTEN))
		if (dump_sk_queue(lfd, ue.id))
			goto err;

	pr_info("Dumping unix socket at %d\n", p->fd);
	show_one_unix("Dumping", sk);
	show_one_unix_img("Dumped", &ue);

	list_del_init(&sk->list);
	sk->sd.already_dumped = 1;
	return 0;

err:
	return -1;
}

int dump_socket(struct fd_parms *p, int lfd, const struct cr_fdset *cr_fdset)
{
	struct socket_desc *sk;

	sk = lookup_socket(p->stat.st_ino);
	if (!sk) {
		pr_err("Uncollected socket 0x%8x\n", (int)p->stat.st_ino);
		return -1;
	}

	switch (sk->family) {
	case AF_UNIX:
		return dump_one_unix(sk, p, lfd, cr_fdset);
	case AF_INET:
	case AF_INET6:
		return dump_one_inet(sk, p, cr_fdset);
	default:
		pr_err("BUG! Unknown socket collected\n");
		break;
	}

	return -1;
}

static int inet_tcp_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

static int inet_udp_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

static int inet_udplite_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET, SOCK_DGRAM, IPPROTO_UDPLITE);
}

static int inet6_tcp_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET6, SOCK_STREAM, IPPROTO_TCP);
}

static int inet6_udp_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
}

static int inet6_udplite_receive_one(struct nlmsghdr *h)
{
	return inet_collect_one(h, AF_INET6, SOCK_DGRAM, IPPROTO_UDPLITE);
}

static int unix_collect_one(const struct unix_diag_msg *m,
		struct rtattr **tb)
{
	struct unix_sk_desc *d, **h;

	d = xzalloc(sizeof(*d));
	if (!d)
		return -1;

	d->type	= m->udiag_type;
	d->state= m->udiag_state;
	INIT_LIST_HEAD(&d->list);

	if (tb[UNIX_DIAG_PEER])
		d->peer_ino = *(int *)RTA_DATA(tb[UNIX_DIAG_PEER]);

	if (tb[UNIX_DIAG_NAME]) {
		int len		= RTA_PAYLOAD(tb[UNIX_DIAG_NAME]);
		char *name	= xmalloc(len + 1);

		if (!name)
			goto err;

		memcpy(name, RTA_DATA(tb[UNIX_DIAG_NAME]), len);
		name[len] = '\0';

		if (name[0] != '\0') {
			struct unix_diag_vfs *uv;
			struct stat st;

			if (name[0] != '/') {
				pr_warn("Relative bind path '%s' "
					"unsupported\n", name);
				xfree(name);
				xfree(d);
				return 0;
			}

			if (!tb[UNIX_DIAG_VFS]) {
				pr_err("Bound socket w/o inode %d\n",
						m->udiag_ino);
				goto err;
			}

			uv = RTA_DATA(tb[UNIX_DIAG_VFS]);
			if (stat(name, &st)) {
				pr_perror("Can't stat socket %d(%s)",
						m->udiag_ino, name);
				goto err;
			}

			if ((st.st_ino != uv->udiag_vfs_ino) ||
			    (st.st_dev != kdev_to_odev(uv->udiag_vfs_dev))) {
				pr_info("unix: Dropping path for "
						"unlinked bound "
						"sk %#x.%#x real %#x.%#x\n",
						(int)st.st_dev,
						(int)st.st_ino,
						(int)uv->udiag_vfs_dev,
						(int)uv->udiag_vfs_ino);
				/*
				 * When a socket is bound to unlinked file, we
				 * just drop his name, since noone will access
				 * it via one.
				 */
				xfree(name);
				len = 0;
				name = NULL;
			}
		}

		d->namelen = len;
		d->name = name;
	}

	if (tb[UNIX_DIAG_ICONS]) {
		int len = RTA_PAYLOAD(tb[UNIX_DIAG_ICONS]);
		int i;

		d->icons = xmalloc(len);
		if (!d->icons)
			goto err;

		memcpy(d->icons, RTA_DATA(tb[UNIX_DIAG_ICONS]), len);
		d->nr_icons = len / sizeof(u32);

		/*
		 * Remember these sockets, we will need them
		 * to fix up in-flight sockets peers.
		 */
		for (i = 0; i < d->nr_icons; i++) {
			struct unix_sk_listen_icon *e, **chain;
			int n;

			e = xzalloc(sizeof(*e));
			if (!e)
				goto err;

			n = d->icons[i];
			chain = &unix_listen_icons[n % SK_HASH_SIZE];
			e->next = *chain;
			*chain = e;

			pr_debug("\t\tCollected icon %d\n", d->icons[i]);

			e->peer_ino	= n;
			e->sk_desc	= d;
		}


	}

	if (tb[UNIX_DIAG_RQLEN]) {
		struct unix_diag_rqlen *rq;

		rq = (struct unix_diag_rqlen *)RTA_DATA(tb[UNIX_DIAG_RQLEN]);
		d->rqlen = rq->udiag_rqueue;
		d->wqlen = rq->udiag_wqueue;
	}

	sk_collect_one(m->udiag_ino, AF_UNIX, &d->sd);
	show_one_unix("Collected", d);

	return 0;

err:
	xfree(d->icons);
	xfree(d->name);
	xfree(d);
	return -1;
}

static int unix_receive_one(struct nlmsghdr *h)
{
	struct unix_diag_msg *m = NLMSG_DATA(h);
	struct rtattr *tb[UNIX_DIAG_MAX+1];

	parse_rtattr(tb, UNIX_DIAG_MAX, (struct rtattr *)(m + 1),
		     h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));

	return unix_collect_one(m, tb);
}

static int collect_sockets_nl(int nl, void *req, int size,
			      int (*receive_callback)(struct nlmsghdr *h))
{
	struct msghdr msg;
	struct sockaddr_nl nladdr;
	struct iovec iov;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name	= &nladdr;
	msg.msg_namelen	= sizeof(nladdr);
	msg.msg_iov	= &iov;
	msg.msg_iovlen	= 1;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family= AF_NETLINK;

	iov.iov_base	= req;
	iov.iov_len	= size;

	if (sendmsg(nl, &msg, 0) < 0) {
		pr_perror("Can't send request message");
		goto err;
	}

	iov.iov_base	= buf;
	iov.iov_len	= sizeof(buf);

	while (1) {
		int err;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name	= &nladdr;
		msg.msg_namelen	= sizeof(nladdr);
		msg.msg_iov	= &iov;
		msg.msg_iovlen	= 1;

		err = recvmsg(nl, &msg, 0);
		if (err < 0) {
			if (errno == EINTR)
				continue;
			else {
				pr_perror("Error receiving nl report");
				goto err;
			}
		}
		if (err == 0)
			break;

		err = nlmsg_receive(buf, err, receive_callback);
		if (err < 0)
			goto err;
		if (err == 0)
			break;
	}

	return 0;

err:
	return -1;
}

int fix_external_unix_sockets(void)
{
	struct unix_sk_desc *sk;
	int i, ret = -1;

	pr_debug("Dumping external sockets\n");

	list_for_each_entry(sk, &unix_sockets, list) {
		struct unix_sk_entry e = { };

		BUG_ON(sk->sd.already_dumped);

		if (!opts.ext_unix_sk) {
			show_one_unix("Runaway socket", sk);
			goto err;
		}

		if (sk->type != SOCK_DGRAM) {
			show_one_unix("Ext stream not supported", sk);
			goto err;
		}

		e.id		= sk->sd.ino;
		e.type		= SOCK_DGRAM;
		e.state		= TCP_LISTEN;
		e.namelen	= sk->namelen;
		e.uflags	= USK_EXTERN;
		e.peer		= 0;

		show_one_unix("Dumping extern", sk);

		if (write_img(fdset_fd(glob_fdset, CR_FD_UNIXSK), &e))
			goto err;
		if (write_img_buf(fdset_fd(glob_fdset, CR_FD_UNIXSK),
					sk->name, e.namelen))
			goto err;

		show_one_unix_img("Dumped extern", &e);
	}

	return 0;
err:
	return -1;
}

int collect_sockets(void)
{
	int err = 0, tmp;
	int nl;
	int supp_type = 0;
	struct {
		struct nlmsghdr hdr;
		union {
			struct unix_diag_req	u;
			struct inet_diag_req_v2	i;
		} r;
	} req;

	nl = socket(PF_NETLINK, SOCK_RAW, NETLINK_SOCK_DIAG);
	if (nl < 0) {
		pr_perror("Can't create sock diag socket");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.hdr.nlmsg_len	= sizeof(req);
	req.hdr.nlmsg_type	= SOCK_DIAG_BY_FAMILY;
	req.hdr.nlmsg_flags	= NLM_F_DUMP | NLM_F_REQUEST;
	req.hdr.nlmsg_seq	= CR_NLMSG_SEQ;

	/* Collect UNIX sockets */
	req.r.u.sdiag_family	= AF_UNIX;
	req.r.u.udiag_states	= -1; /* All */
	req.r.u.udiag_show	= UDIAG_SHOW_NAME | UDIAG_SHOW_VFS |
				  UDIAG_SHOW_PEER | UDIAG_SHOW_ICONS |
				  UDIAG_SHOW_RQLEN;
	tmp = collect_sockets_nl(nl, &req, sizeof(req), unix_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv4 TCP sockets */
	req.r.i.sdiag_family	= AF_INET;
	req.r.i.sdiag_protocol	= IPPROTO_TCP;
	req.r.i.idiag_ext	= 0;
	/* Only listening sockets supported yet */
	req.r.i.idiag_states	= 1 << TCP_LISTEN;
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet_tcp_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv4 UDP sockets */
	req.r.i.sdiag_family	= AF_INET;
	req.r.i.sdiag_protocol	= IPPROTO_UDP;
	req.r.i.idiag_ext	= 0;
	req.r.i.idiag_states	= -1; /* All */
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet_udp_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv4 UDP-lite sockets */
	req.r.i.sdiag_family	= AF_INET;
	req.r.i.sdiag_protocol	= IPPROTO_UDPLITE;
	req.r.i.idiag_ext	= 0;
	req.r.i.idiag_states	= -1; /* All */
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet_udplite_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv6 TCP sockets */
	req.r.i.sdiag_family	= AF_INET6;
	req.r.i.sdiag_protocol	= IPPROTO_TCP;
	req.r.i.idiag_ext	= 0;
	/* Only listening sockets supported yet */
	req.r.i.idiag_states	= 1 << TCP_LISTEN;
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet6_tcp_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv6 UDP sockets */
	req.r.i.sdiag_family	= AF_INET6;
	req.r.i.sdiag_protocol	= IPPROTO_UDP;
	req.r.i.idiag_ext	= 0;
	req.r.i.idiag_states	= -1; /* All */
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet6_udp_receive_one);
	if (tmp)
		err = tmp;

	/* Collect IPv6 UDP-lite sockets */
	req.r.i.sdiag_family	= AF_INET6;
	req.r.i.sdiag_protocol	= IPPROTO_UDPLITE;
	req.r.i.idiag_ext	= 0;
	req.r.i.idiag_states	= -1; /* All */
	tmp = collect_sockets_nl(nl, &req, sizeof(req), inet6_udplite_receive_one);
	if (tmp)
		err = tmp;
out:
	close(nl);
	return err;
}

struct unix_sk_info {
	struct unix_sk_entry ue;
	struct list_head list;
	char *name;
	unsigned flags;
	struct unix_sk_info *peer;
	struct file_desc d;
};

#define USK_PAIR_MASTER		0x1
#define USK_PAIR_SLAVE		0x2

static struct unix_sk_info *find_unix_sk(int id)
{
	struct file_desc *d;

	d = find_file_desc_raw(FDINFO_UNIXSK, id);
	if (d)
		return container_of(d, struct unix_sk_info, d);
	return NULL;
}

static inline char *unknown(u32 val)
{
	static char unk[12];
	snprintf(unk, sizeof(unk), "x%d", val);
	return unk;
}

char *skfamily2s(u32 f)
{
	if (f == AF_INET)
		return " inet";
	else if (f == AF_INET6)
		return "inet6";
	else
		return unknown(f);
}

char *sktype2s(u32 t)
{
	if (t == SOCK_STREAM)
		return "stream";
	else if (t == SOCK_DGRAM)
		return " dgram";
	else
		return unknown(t);
}

char *skproto2s(u32 p)
{
	if (p == IPPROTO_UDP)
		return "udp";
	else if (p == IPPROTO_UDPLITE)
		return "udpl";
	else if (p == IPPROTO_TCP)
		return "tcp";
	else
		return unknown(p);
}

char *skstate2s(u32 state)
{
	if (state == TCP_ESTABLISHED)
		return " estab";
	else if (state == TCP_CLOSE)
		return "closed";
	else if (state == TCP_LISTEN)
		return "listen";
	else
		return unknown(state);
}

void show_unixsk(int fd, struct cr_options *o)
{
	struct unix_sk_entry ue;
	int ret = 0;

	pr_img_head(CR_FD_UNIXSK);

	while (1) {
		ret = read_img_eof(fd, &ue);
		if (ret <= 0)
			goto out;

		pr_msg("id 0x%8x type %s state %s namelen %4d backlog %4d peer 0x%8x flags 0x%2x uflags 0x%2x",
			ue.id, sktype2s(ue.type), skstate2s(ue.state),
			ue.namelen, ue.backlog, ue.peer, ue.flags, ue.uflags);

		if (ue.namelen) {
			BUG_ON(ue.namelen > sizeof(buf));
			ret = read_img_buf(fd, buf, ue.namelen);
			if (ret < 0) {
				pr_info("\n");
				goto out;
			}
			if (!buf[0])
				buf[0] = '@';
			pr_msg(" --> %s\n", buf);
		} else
			pr_msg("\n");
		pr_msg("\t"), show_fown_cont(&ue.fown), pr_msg("\n");
	}
out:
	pr_img_tail(CR_FD_UNIXSK);
}

struct unix_conn_job {
	struct unix_sk_info	*sk;
	struct unix_conn_job	*next;
};

static struct unix_conn_job *conn_jobs;

static int schedule_conn_job(struct unix_sk_info *ui)
{
	struct unix_conn_job *cj;

	cj = xmalloc(sizeof(*cj));
	if (!cj)
		return -1;

	cj->sk = ui;
	cj->next = conn_jobs;
	conn_jobs = cj;

	return 0;
}

int run_unix_connections(void)
{
	struct unix_conn_job *cj;

	pr_info("Running delayed unix connections\n");

	cj = conn_jobs;
	while (cj) {
		int attempts = 8;
		struct unix_sk_info *ui = cj->sk;
		struct unix_sk_info *peer = ui->peer;
		struct fdinfo_list_entry *fle;
		struct sockaddr_un addr;

		pr_info("\tConnect %#x to %#x\n", ui->ue.id, peer->ue.id);

		fle = file_master(&ui->d);

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		memcpy(&addr.sun_path, peer->name, peer->ue.namelen);
try_again:
		if (connect(fle->fe.fd, (struct sockaddr *)&addr,
					sizeof(addr.sun_family) +
					peer->ue.namelen) < 0) {
			if (attempts) {
				usleep(1000);
				attempts--;
				goto try_again; /* FIXME use futex waiters */
			}

			pr_perror("Can't connect %#x socket", ui->ue.id);
			return -1;
		}

		if (restore_sk_queue(fle->fe.fd, peer->ue.id))
			return -1;

		if (rst_file_params(fle->fe.fd, &ui->ue.fown, ui->ue.flags))
			return -1;

		cj = cj->next;
	}

	return 0;
}

static int bind_unix_sk(int sk, struct unix_sk_info *ui)
{
	struct sockaddr_un addr;

	if ((ui->ue.type == SOCK_STREAM) && (ui->ue.state != TCP_LISTEN))
		/*
		 * FIXME this can be done, but for doing this properly we
		 * need to bind socket to its name, then rename one to
		 * some temporary unique one and after all the sockets are
		 * restored we should walk those temp names and rename
		 * some of them back to real ones.
		 */
		goto done;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(&addr.sun_path, ui->name, ui->ue.namelen);

	if (bind(sk, (struct sockaddr *)&addr,
				sizeof(addr.sun_family) + ui->ue.namelen)) {
		pr_perror("Can't bind socket");
		return -1;
	}
done:
	return 0;
}

static int unixsk_should_open_transport(struct fdinfo_entry *fe,
				struct file_desc *d)
{
	struct unix_sk_info *ui;

	ui = container_of(d, struct unix_sk_info, d);
	return ui->flags & USK_PAIR_SLAVE;
}

static int open_unixsk_pair_master(struct unix_sk_info *ui)
{
	int sk[2], tsk;
	struct unix_sk_info *peer = ui->peer;
	struct fdinfo_list_entry *fle;

	pr_info("Opening pair master (id %#x peer %#x)\n",
			ui->ue.id, ui->ue.peer);

	if (socketpair(PF_UNIX, ui->ue.type, 0, sk) < 0) {
		pr_perror("Can't make socketpair");
		return -1;
	}

	if (restore_sk_queue(sk[0], peer->ue.id))
		return -1;
	if (restore_sk_queue(sk[1], ui->ue.id))
		return -1;

	if (bind_unix_sk(sk[0], ui))
		return -1;

	if (rst_file_params(sk[0], &ui->ue.fown, ui->ue.flags))
		return -1;

	tsk = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (tsk < 0) {
		pr_perror("Can't make transport socket");
		return -1;
	}

	fle = file_master(&peer->d);
	if (send_fd_to_peer(sk[1], fle, tsk)) {
		pr_err("Can't send pair slave\n");
		return -1;
	}

	close(tsk);
	close(sk[1]);

	return sk[0];
}

static int open_unixsk_pair_slave(struct unix_sk_info *ui)
{
	struct fdinfo_list_entry *fle;
	int sk;

	fle = file_master(&ui->d);

	pr_info("Opening pair slave (id %#x peer %#x) on %d\n",
			ui->ue.id, ui->ue.peer, fle->fe.fd);

	sk = recv_fd(fle->fe.fd);
	if (sk < 0) {
		pr_err("Can't recv pair slave");
		return -1;
	}
	close(fle->fe.fd);

	if (bind_unix_sk(sk, ui))
		return -1;

	if (rst_file_params(sk, &ui->ue.fown, ui->ue.flags))
		return -1;

	return sk;
}

static int open_unixsk_standalone(struct unix_sk_info *ui)
{
	int sk;

	pr_info("Opening standalone socket (id %#x peer %#x)\n",
			ui->ue.id, ui->ue.peer);

	sk = socket(PF_UNIX, ui->ue.type, 0);
	if (sk < 0) {
		pr_perror("Can't make unix socket");
		return -1;
	}

	if (bind_unix_sk(sk, ui))
		return -1;

	if (ui->ue.state == TCP_LISTEN) {
		pr_info("\tPutting %#x into listen state\n", ui->ue.id);
		if (listen(sk, ui->ue.backlog) < 0) {
			pr_perror("Can't make usk listen");
			return -1;
		}

		if (rst_file_params(sk, &ui->ue.fown, ui->ue.flags))
			return -1;
	} else if (ui->peer) {
		pr_info("\tWill connect %#x to %#x later\n", ui->ue.id, ui->ue.peer);
		if (schedule_conn_job(ui))
			return -1;
	}

	return sk;
}

static int open_unix_sk(struct file_desc *d)
{
	struct unix_sk_info *ui;

	ui = container_of(d, struct unix_sk_info, d);
	if (ui->flags & USK_PAIR_MASTER)
		return open_unixsk_pair_master(ui);
	else if (ui->flags & USK_PAIR_SLAVE)
		return open_unixsk_pair_slave(ui);
	else
		return open_unixsk_standalone(ui);
}

static struct file_desc_ops unix_desc_ops = {
	.open = open_unix_sk,
	.want_transport = unixsk_should_open_transport,
};

int collect_unix_sockets(void)
{
	int fd, ret;

	pr_info("Reading unix sockets in\n");

	fd = open_image_ro(CR_FD_UNIXSK);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		else
			return -1;
	}

	while (1) {
		struct unix_sk_info *ui;

		ui = xmalloc(sizeof(*ui));
		ret = -1;
		if (ui == NULL)
			break;

		ret = read_img_eof(fd, &ui->ue);
		if (ret <= 0) {
			xfree(ui);
			break;
		}

		if (ui->ue.namelen) {
			ret = -1;

			if (!ui->ue.namelen || ui->ue.namelen >= UNIX_PATH_MAX) {
				pr_err("Bad unix name len %d\n", ui->ue.namelen);
				break;
			}

			ui->name = xmalloc(ui->ue.namelen);
			if (ui->name == NULL)
				break;

			ret = read_img_buf(fd, ui->name, ui->ue.namelen);
			if (ret < 0)
				break;

			/*
			 * Make FS clean from sockets we're about to
			 * restore. See for how we bind them for details
			 */
			if (ui->name[0] != '\0' &&
			    !(ui->ue.uflags & USK_EXTERN))
				unlink(ui->name);
		} else
			ui->name = NULL;

		ui->peer = NULL;
		ui->flags = 0;
		pr_info(" `- Got %u peer %u\n", ui->ue.id, ui->ue.peer);
		file_desc_add(&ui->d, FDINFO_UNIXSK, ui->ue.id,
				&unix_desc_ops);
		list_add_tail(&ui->list, &unix_sockets);
	}

	close(fd);

	return read_sk_queues();
}

int resolve_unix_peers(void)
{
	struct unix_sk_info *ui, *peer;
	struct fdinfo_list_entry *fle, *fle_peer;

	list_for_each_entry(ui, &unix_sockets, list) {
		if (ui->peer)
			continue;
		if (!ui->ue.peer)
			continue;

		peer = find_unix_sk(ui->ue.peer);

		/*
		 * Connect to external sockets requires
		 * special option to be passed.
		 */
		if (peer &&
		    (peer->ue.uflags & USK_EXTERN) &&
		    !(opts.ext_unix_sk))
			peer = NULL;

		if (!peer) {
			pr_err("FATAL: Peer %#x unresolved for %#x\n",
					ui->ue.peer, ui->ue.id);
			return -1;
		}

		ui->peer = peer;
		if (ui == peer)
			/* socket connected to self %) */
			continue;
		if (peer->ue.peer != ui->ue.id)
			continue;

		/* socketpair or interconnected sockets */
		peer->peer = ui;

		/*
		 * Select who will restore the pair. Check is identical to
		 * the one in pipes.c and makes sure tasks wait for each other
		 * in pids sorting order (ascending).
		 */

		fle = file_master(&ui->d);
		fle_peer = file_master(&peer->d);

		if ((fle->pid < fle_peer->pid) ||
				(fle->pid == fle_peer->pid &&
				 fle->fe.fd < fle_peer->fe.fd)) {
			ui->flags |= USK_PAIR_MASTER;
			peer->flags |= USK_PAIR_SLAVE;
		} else {
			peer->flags |= USK_PAIR_MASTER;
			ui->flags |= USK_PAIR_SLAVE;
		}
	}

	pr_info("Unix sockets:\n");
	list_for_each_entry(ui, &unix_sockets, list) {
		struct fdinfo_list_entry *fle;

		pr_info("\t%#x -> %#x (%#x) flags %#x\n", ui->ue.id, ui->ue.peer,
				ui->peer ? ui->peer->ue.id : 0, ui->flags);
		list_for_each_entry(fle, &ui->d.fd_info_head, desc_list)
			pr_info("\t\tfd %d in pid %d\n",
					fle->fe.fd, fle->pid);

	}

	return 0;
}
