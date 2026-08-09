import socket
import struct
import time
import argparse
import sys

MAGIC = b"MDVA_STATE_123\x00\x00"

def send_cmd(s, cmd):
    s.sendall(cmd.encode('ascii') + b'\n')

def read_mem_until_newline(s, cmd):
    send_cmd(s, cmd)
    data = bytearray()
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
        if b'\n' in chunk:
            break
    try:
        return bytes.fromhex(data.decode('ascii').strip())
    except:
        return b""

def read_main(s, offset, size):
    return read_mem_until_newline(s, f"peekMain 0x{offset:X} {size}")

def read_absolute(s, addr, size):
    return read_mem_until_newline(s, f"peekAbsolute 0x{addr:X} {size}")

def start_prototype_server(switch_ip):
    print(f"Connecting to sys-botbase on {switch_ip}:6000...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((switch_ip, 6000))
    except Exception as e:
        print(f"Failed to connect: {e}")
        print("Make sure sys-botbase is installed and the Switch is on the same network.")
        return

    print("Connected! Locating mod in memory...")

    # The game's ADDR_FIND_SCORE is at Ghidra offset 0x0C7990.
    # But because of the 0x100 byte NSO header, it's actually loaded at main NSO + 0x0C7890.
    # Exlaunch replaces it with a trampoline:
    # 0x0C7890: ldr x16, 8
    # 0x0C7894: br x16
    # 0x0C7898: <8-byte absolute address of FindScoreHook::Callback>
    # By reading main NSO + 0x0C7898, we can get an absolute pointer into our mod!
    
    hook_ptr_data = read_main(s, 0x0C7898, 8)
    
    if not hook_ptr_data or len(hook_ptr_data) < 8:
        print("Failed to read hook pointer using peekMain. Is the game running and the mod installed?")
        return

    mod_func_addr = struct.unpack('<Q', hook_ptr_data)[0]
    print(f"Mod function found at absolute address: 0x{mod_func_addr:X}")

    if mod_func_addr == 0:
        print("Pointer is null. Exlaunch hook might not be installed yet.")
        return

    # Align down to page boundary to find mod base roughly
    search_base = mod_func_addr & ~0xFFF
    print(f"Starting memory scan for magic signature near 0x{search_base:X}...")

    # Scan 16MB around the mod base for the magic signature
    state_addr = 0
    chunk_size = 0x40000
    for offset in range(-0x800000, 0x800000, chunk_size):
        addr = search_base + offset
        data = read_absolute(s, addr, chunk_size)
        idx = data.find(MAGIC)
        if idx != -1:
            state_addr = addr + idx
            print(f"Found MultiplayerState at 0x{state_addr:X}!")
            break
    
    if state_addr == 0:
        print("Could not find MultiplayerState struct in memory.")
        return

    print("\n--- Listening for Game State ---\n")
    
    last_song_started = 0
    
    while True:
        try:
            # Read the state struct (40 bytes)
            state_data = read_absolute(s, state_addr, 40)
            if len(state_data) < 40:
                time.sleep(1)
                continue
                
            # Struct layout: 
            # char magic[16] (0-15)
            # uint32 isGameOpen (16-19)
            # uint32 isSongStarted (20-23)
            # uint32 currentPvId (24-27)
            # padding (28-31)
            # uint64 currentScorePtr (32-39)
            
            magic, is_open, is_song_started, pv_id, score_ptr = struct.unpack('<16sIII4xQ', state_data)
            
            if magic.strip(b'\x00') != b"MDVA_STATE_123":
                print("WARNING: Magic signature not found. Did the address change?")
                time.sleep(5)
                continue
            
            if pv_id != last_song_started:
                if pv_id > 0:
                    print(f"SONG SELECTED / PLAYING! PV ID: {pv_id}")
                    if score_ptr != 0:
                        print(f"Saved Score Struct located at: 0x{score_ptr:X}")
                
            last_song_started = pv_id
            
            time.sleep(0.5) # Poll twice a second
            
        except Exception as e:
            print(f"Connection lost: {e}")
            break

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="MegaMix Multiplayer Prototype Server")
    parser.add_argument("ip", help="IP address of the Nintendo Switch")
    args = parser.parse_args()
    
    start_prototype_server(args.ip)