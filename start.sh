#!/bin/bash

# 日志目录
LOGDIR="logs/$(date +%F_%H-%M-%S)"
mkdir -p "$LOGDIR"

# 带时间戳的日志记录包装器
log_with_time() {
    local name="$1"
    local file="$LOGDIR/${name}.log"
    shift
    "$@" 2>&1 | while IFS= read -r line; do
        echo "$(date +%H:%M:%S.%3N) [$name] $line" >>"$file"
    done &
}

# 退出时自动清理所有后台进程
_cleaning=
cleanup() {
    [ -n "$_cleaning" ] && return; _cleaning=1
    echo "Stopping all services... logs saved to $LOGDIR"
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
}
trap cleanup SIGINT SIGTERM EXIT

# 激活虚拟环境
source venv/bin/activate

# 启动 signaling server
log_with_time signaling python3 signaling_server.py

# 启动 http server (提供前端页面)
log_with_time http python -m http.server 8000

# 等待 signaling server 启动
sleep 1

# 启动 WebRTC 推流 + 人脸检测
log_with_time webrtc ./webrtc_send_ws

echo "All services started. Logs: $LOGDIR"
echo "  tail -f $LOGDIR/signaling.log"
echo "  tail -f $LOGDIR/webrtc.log"
echo "  tail -f $LOGDIR/http.log"

# 等待所有后台任务
wait
