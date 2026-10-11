/*
 * localoptions.h - dropbear's build options for CosmoOS (roadmap M4,
 * decision 6; ports/dropbear/README.md). Copied into the source tree
 * before the build; each line overrides src/default_options.h.
 *
 * Public-key authentication only: password authentication is compiled
 * out, not merely refused. One host key type, Ed25519: generated at the
 * installed system's first boot (dropbearkey, from the seeded pool), and
 * the one OpenSSH clients prefer. What an ssh login needs and nothing
 * else: no forwarding of any kind, no sftp server (scp/sftp are outside
 * M4), no X11, no MOTD, no inetd mode.
 */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1

#define DROPBEAR_RSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_SK_KEYS 0
#define ED25519_PRIV_FILENAME "/etc/dropbear/dropbear_ed25519_host_key"

#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_SFTPSERVER 0
#define DO_MOTD 0
#define INETD_MODE 0

/* The machine's PATH (init's); root's is the same. */
#define DEFAULT_PATH "/bin:/sbin:/usr/bin:/usr/sbin"
#define DEFAULT_ROOT_PATH "/bin:/sbin:/usr/bin:/usr/sbin"
