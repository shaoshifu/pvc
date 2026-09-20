# -*- coding: utf-8 -*-
"""服务器操作小工具：把命令发到远端跑，实时回显。

为什么不用 sshpass + ssh：
  本机没有 sshpass；而 paramiko 已在（Python 3.14 自带环境里有）。
  另外 paramiko 能拿到 stdout/stderr 分离的输出与退出码，比 ssh 的文本流更好判断。
"""
import sys, paramiko

HOST = "111.229.27.13"
USER = "root"
PWD  = "Asd266999799979997"

def run(cmd, timeout=1800, quiet=False):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PWD, timeout=25,
              allow_agent=False, look_for_keys=False)
    try:
        # 用 bash -lc 保证 PATH 与交互式登录一致（emsdk_env.sh 依赖这个）
        i, o, e = c.exec_command(cmd, timeout=timeout, get_pty=False)
        out = o.read().decode('utf-8', 'replace')
        err = e.read().decode('utf-8', 'replace')
        rc = o.channel.recv_exit_status()
        if not quiet:
            if out: sys.stdout.write(out)
            if err: sys.stderr.write(err)
        return rc, out, err
    finally:
        c.close()

def put(local, remote):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PWD, timeout=25,
              allow_agent=False, look_for_keys=False)
    try:
        s = c.open_sftp()
        s.put(local, remote)
        s.close()
    finally:
        c.close()

if __name__ == "__main__":
    cmd = sys.stdin.read() if len(sys.argv) < 2 else " ".join(sys.argv[1:])
    rc, o, e = run(cmd)
    print("\n[exit=%d]" % rc)
