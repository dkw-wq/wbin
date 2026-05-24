import asyncio
import websockets

# 存储所有连接的客户端
connected = set()

async def handler(websocket):
    # 1. 新客户端连接
    connected.add(websocket)
    print(f"New connection. Total: {len(connected)}")
    
    try:
        async for message in websocket:
            # 2. 收到消息，转发给所有其他人
            for conn in connected:
                if conn != websocket:  # 不发回给自己
                    await conn.send(message)
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        # 3. 客户端断开
        connected.remove(websocket)
        print(f"Connection closed. Remaining: {len(connected)}")

async def main():
    # 使用 0.0.0.0 允许局域网内的其他设备（你的电脑/手机）连接
    print("Signaling server started on ws://0.0.0.0:8443")
    
    # 启动服务器，并保持运行
    async with websockets.serve(handler, "0.0.0.0", 8443):
        await asyncio.Future()  # 这是一个永远不会完成的 Future，相当于 run_forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped.")