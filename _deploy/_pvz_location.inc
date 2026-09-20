    # === 植物大战僵尸 网页版（WASM 静态站）===
    # 由 /root/pvc 编译产出（emsdk 3.1.61），部署在 /var/www/pvc。
    #
    # 用 ^~ 前缀：确保 /pvz/xxx 不被下面的 location / （catch-all 反代 3000）抢走。
    #
    # ⚠️ 这里**刻意不嵌套正则 location**。第一版写了
    #      location ~* \.(data|wasm)$ { alias /var/www/pvc/; ... }
    #    这是错的：正则 location 里的 alias **不带路径前缀**，
    #    nginx 会把 $uri（已经是 /pvz/pvz.wasm）再拼一次 →
    #    /var/www/pvc//pvz/pvz.wasm → 不存在 → 301 到目录形式 → 前端报错。
    #    正确做法就是让父级别名统一处理（前缀 location 的 alias 语义不同，那个是对的）。
    #
    # alias 结尾的斜杠必须与 location 前缀的斜杠对应，否则路径会拼错。
    location ^~ /pvz/ {
        alias /var/www/pvc/;
        index index.html;
        autoindex off;
        # 76MB 的 pvz.data 与 416KB 的 pvz.wasm 都是大文件；
        # 不做特殊缓存头（默认启发式缓存即可）——
        # 重新编译后文件名不变，缓存太久反而会拿到旧版本。
    }

