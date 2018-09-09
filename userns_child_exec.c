/* userns_child_exec.c

   GNU General Public License v2 以降の元でライセンスされる

   新しい名前空間でシェルコマンドを実行する子プロセスを作成する。
   ユーザー名前空間を作成する際に UID と GID のマッピングを
   指定することができる。
*/
#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

/* 簡単なエラー処理関数: \(aqerrno\(aq の値に基づいて
   エラーメッセージを出力し、呼び出し元プロセスを終了する。 */

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                        } while (0)

struct child_args {
    char **argv;        /* 子プロセスが実行するコマンドと引き数 */
    int    pipe_fd[2];  /* 親プロセスと子プロセスを同期するためのパイプ */
};

static int verbose;

static void
usage(char *pname)
{
    fprintf(stderr, "Usage: %s [options] cmd [arg...]\n\n", pname);
    fprintf(stderr, "Create a child process that executes a shell "
            "command in a new user namespace,\n"
            "and possibly also other new namespace(s).\n\n");
    fprintf(stderr, "Options can be:\n\n");
#define fpe(str) fprintf(stderr, "    %s", str);
    fpe("-i          New IPC namespace\n");
    fpe("-m          New mount namespace\n");
    fpe("-n          New network namespace\n");
    fpe("-p          New PID namespace\n");
    fpe("-u          New UTS namespace\n");
    fpe("-U          New user namespace\n");
    fpe("-M uid_map  Specify UID map for user namespace\n");
    fpe("-G gid_map  Specify GID map for user namespace\n");
    fpe("-z          Map user's UID and GID to 0 in user namespace\n");
    fpe("            (equivalent to: -M '0 <uid> 1' -G '0 <gid> 1')\n");
    fpe("-v          Display verbose messages\n");
    fpe("\n");
    fpe("If -z, -M, or -G is specified, -U is required.\n");
    fpe("It is not permitted to specify both -z and either -M or -G.\n");
    fpe("\n");
    fpe("Map strings for -M and -G consist of records of the form:\n");
    fpe("\n");
    fpe("    ID-inside-ns   ID-outside-ns   len\n");
    fpe("\n");
    fpe("A map string can contain multiple records, separated"
        " by commas;\n");
    fpe("the commas are replaced by newlines before writing"
        " to map files.\n");

    exit(EXIT_FAILURE);
}

/* マッピングファイル 'map_file' を 'mapping' で指定
   された値で更新する。 'mapping' は UID や GID マッピングを
   定義する文字列である。 UID や GID マッピングは以下の形式の改行
   で区切られた 1 つ以上のレコードである。

       NS 内 ID        NS 外 ID        長さ

   ユーザーに改行を含む文字列を指定するのを求めるのは、
   コマンドラインを使う場合にはもちろん不便なことである。
   そのため、 この文字列でレコードを区切るのにカンマを
   使えるようにして、ファイルにこの文字列を書き込む前に
   カンマを改行に置換する。 */

static void
update_map(char *mapping, char *map_file)
{
    int fd, j;
    size_t map_len;     /* 'mapping' の長さ */

    /* マッピング文字列内のカンマを改行で置換する */

    map_len = strlen(mapping);
    for (j = 0; j < map_len; j++)
        if (mapping[j] == ',')
            mapping[j] = '\n';

    fd = open(map_file, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "ERROR: open %s: %s\n", map_file,
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (write(fd, mapping, map_len) != map_len) {
        fprintf(stderr, "ERROR: write %s: %s\n", map_file,
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    close(fd);
}

static int              /* クローンされた子プロセスの開始関数 */
childFunc(void *arg)
{
    struct child_args *args = (struct child_args *) arg;
    char ch;

    /* 親プロセスが UID と GID マッピングを更新するまで待つ。
       main() のコメントを参照。 パイプの end of file を待つ。
       親プロセスが一旦マッピングを更新すると、
       パイプはクローズされる。 */

    close(args->pipe_fd[1]);    /* パイプのこちら側の書き込み端のディスク
                                       リプターをクローズする。これにより
                                       親プロセスがディスクリプターをクローズ
                                       すると EOF が見えるようになる。 */
    if (read(args->pipe_fd[0], &ch, 1) != 0) {
        fprintf(stderr,
                "Failure in child: read from pipe returned != 0\n");
        exit(EXIT_FAILURE);
    }

    /* シェルコマンドを実行する */

    printf("About to exec %s\n", args->argv[0]);
    execvp(args->argv[0], args->argv);
    errExit("execvp");
}

#define STACK_SIZE (1024 * 1024)

static char child_stack[STACK_SIZE];    /* 子プロセスのスタック空間 */

int
main(int argc, char *argv[])
{
    int flags, opt, map_zero;
    pid_t child_pid;
    struct child_args args;
    char *uid_map, *gid_map;
    const int MAP_BUF_SIZE = 100;
    char map_buf[MAP_BUF_SIZE];
    char map_path[PATH_MAX];

    /* コマンドラインオプションを解析する。
       最後の getopt() 引き数の最初の '+' 文字は
       GNU 風のコマンドラインオプションの並び換えを防止する。
       このプログラム自身が実行する「コマンド」にコマンドライン
       オプションが含まれる場合があるからである。
       getopt() にこれらをこのプログラムのオプションとして
       扱ってほしくはないのだ。 */

    flags = 0;
    verbose = 0;
    gid_map = NULL;
    uid_map = NULL;
    map_zero = 0;
    while ((opt = getopt(argc, argv, "+imnpuUM:G:zv")) != -1) {
        switch (opt) {
        case 'i': flags |= CLONE_NEWIPC;        break;
        case 'm': flags |= CLONE_NEWNS;         break;
        case 'n': flags |= CLONE_NEWNET;        break;
        case 'p': flags |= CLONE_NEWPID;        break;
        case 'u': flags |= CLONE_NEWUTS;        break;
        case 'v': verbose = 1;                  break;
        case 'z': map_zero = 1;                 break;
        case 'M': uid_map = optarg;             break;
        case 'G': gid_map = optarg;             break;
        case 'U': flags |= CLONE_NEWUSER;       break;
        default:  usage(argv[0]);
        }
    }

    /* -U なしの -M や -G の指定は意味がない */

    if (((uid_map != NULL || gid_map != NULL || map_zero) &&
                !(flags & CLONE_NEWUSER)) ||
            (map_zero && (uid_map != NULL || gid_map != NULL)))
        usage(argv[0]);

    args.argv = &argv[optind];

    /* 親プログラムと子プロセスを同期するためにパイプを使っている。
       これは、子プロセスが execve() を呼び出す前に、親プロセスにより
       UID と GID マップが設定されることを保証するためである。
       これにより、新しいユーザー名前空間において子プロセスの実効
       ユーザー ID を 0 にマッピングしたいという通常の状況で、
       子プロセスが execve() 実行中にそのケーパビリティを維持する
       ことができる。 この同期を行わないと、 0 以外のユーザー ID で
       execve() を実行した際に、子プロセスがそのケーパビリティを失う
       ことになる (execve() 実行中のプロセスのケーパビリティの変化の
       詳細については capabilities(7) マニュアルページを参照)。 */

    if (pipe(args.pipe_fd) == -1)
        errExit("pipe");

    /* 新しい名前空間で子プロセスを作成する */

    child_pid = clone(childFunc, child_stack + STACK_SIZE,
                      flags | SIGCHLD, &args);
    if (child_pid == -1)
        errExit("clone");

    /* 親プロセスはここを実行する */

    if (verbose)
        printf("%s: PID of child created by clone() is %ld\n",
                argv[0], (long) child_pid);

    /* 子プロセスの UID と GID のマッピングを更新する */

    if (uid_map != NULL || map_zero) {
        snprintf(map_path, PATH_MAX, "/proc/%ld/uid_map",
                (long) child_pid);
        if (map_zero) {
            snprintf(map_buf, MAP_BUF_SIZE, "0 %ld 1", (long) getuid());
            uid_map = map_buf;
        }
        update_map(uid_map, map_path);
    }
    if (gid_map != NULL || map_zero) {
        snprintf(map_path, PATH_MAX, "/proc/%ld/gid_map",
                (long) child_pid);
        if (map_zero) {
            snprintf(map_buf, MAP_BUF_SIZE, "0 %ld 1", (long) getgid());
            gid_map = map_buf;
        }
        update_map(gid_map, map_path);
    }

    /* パイプの書き込み端をクローズし、子プロセスに UID と GID の
       マッピングが更新されたことを知らせる */

    close(args.pipe_fd[1]);

    if (waitpid(child_pid, NULL, 0) == -1)      /* 子プロセスを待つ */
        errExit("waitpid");

    if (verbose)
        printf("%s: terminating\n", argv[0]);

    exit(EXIT_SUCCESS);
}
