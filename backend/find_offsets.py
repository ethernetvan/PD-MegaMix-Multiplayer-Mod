import socket
import struct
import sys
import time
import os

MAGIC = b"MDVA_STATE_123"

def send_cmd(s, cmd):
    s.sendall(cmd)

def read_absolute(s, addr, size):
    send_cmd(s, f"peekAbsolute 0x{addr:X} {size}\n".encode('ascii'))
    
    data = bytearray()
    while len(data) < size * 2:
        chunk = s.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
        if b'\n' in chunk:
            break
            
    hex_str = data.decode('ascii', errors='ignore').strip()
    try:
        return bytes.fromhex(hex_str)
    except:
        return b""

def poke_absolute(s, addr, data_bytes):
    hex_str = data_bytes.hex()
    send_cmd(s, f"pokeAbsolute 0x{addr:X} {hex_str}\n".encode('ascii'))

def find_multiplayer_state(s):
    # Ask sys-botbase for main base (same logic as prototype_server)
    send_cmd(s, b"peekMain 0x0C7898 8\n")
    
    data = bytearray()
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
        if b'\n' in chunk:
            break
            
    hex_str = data.decode('ascii', errors='ignore').strip()
    
    try:
        byte_data = bytes.fromhex(hex_str)
        hook_addr = struct.unpack('<Q', byte_data)[0]
        
        if hook_addr == 0:
            print("Pointer is null. Mod might not be installed.")
            return 0, 0
        search_base = hook_addr & ~0xFFF
    except Exception as e:
        print(f"DEBUG: Exception during peekMain: {e}")
        return 0, 0
        
    print(f"Starting memory scan for magic signature near 0x{search_base:X}...")
    chunk_size = 0x40000
    for offset in range(-0x800000, 0x800000, chunk_size):
        addr = search_base + offset
        data = read_absolute(s, addr, chunk_size)
        
        idx = 0
        while True:
            idx = data.find(MAGIC, idx)
            if idx == -1: break
                
            candidate_addr = addr + idx
            struct_data = data[idx:idx+40] if idx + 40 <= len(data) else read_absolute(s, candidate_addr, 40)
            
            if len(struct_data) >= 40:
                magic_bytes, is_open = struct.unpack('<16sI', struct_data[:20])
                if is_open == 1:
                    return candidate_addr, search_base
                    
            idx += len(MAGIC)
            
    return 0, search_base

def main():
    if len(sys.argv) < 3:
        print("Usage: python find_offsets.py <SWITCH_IP> <CURRENT_PV_ID>")
        print("Example: python find_offsets.py 192.168.1.72 257")
        return

    ip = sys.argv[1]
    target_val = int(sys.argv[2])
    
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((ip, 6000))
        print("Connected to sys-botbase!")
    except Exception as e:
        print(f"Failed to connect: {e}")
        return

    print("Locating C++ Mod in memory...")
    state_addr, search_base = find_multiplayer_state(s)
    
    if state_addr == 0:
        print("ERROR: Could not find MultiplayerState struct in memory. Are you running the latest NSO?")
        return
        
    print(f"Found MultiplayerState at 0x{state_addr:X}")
    
    # Calculate field offsets based on MultiplayerState struct
    # char magic[16]; -> 0x00
    # uint32_t isGameOpen; -> 0x10
    # uint32_t isSongStarted; -> 0x14
    # uint32_t currentPvId; -> 0x18
    # void* currentScorePtr; -> 0x20
    # uint32_t scanTarget; -> 0x28
    # uint32_t scanReset; -> 0x2C
    # int32_t matchCount; -> 0x30
    # uint64_t matches[1000]; -> 0x38
    
    addr_scanTarget = state_addr + 0x28
    addr_scanReset = state_addr + 0x2C
    addr_matchCount = state_addr + 0x30
    addr_matches = state_addr + 0x38

    matches_file = "matches.txt"
    is_reset = 1
    
    if os.path.exists(matches_file):
        print(f"Found existing matches file '{matches_file}'. Filtering previous results...")
        is_reset = 0
    else:
        print("Starting fresh memory scan... (This will execute natively on the Switch at 60FPS!)")
        
    # Trigger the C++ scan!
    poke_absolute(s, addr_scanReset, struct.pack('<I', is_reset))
    poke_absolute(s, addr_scanTarget, struct.pack('<I', target_val))
    
    print("Waiting for C++ mod to finish scan...")
    # Wait until scanTarget becomes 0
    while True:
        status_data = read_absolute(s, addr_scanTarget, 4)
        if len(status_data) == 4:
            status = struct.unpack('<I', status_data)[0]
            if status == 0:
                break
        time.sleep(0.1)
        
    print("Scan complete! Reading results...")
    
    # Read matchCount
    count_data = read_absolute(s, addr_matchCount, 4)
    match_count = struct.unpack('<i', count_data)[0]
    
    print(f"Found {match_count} matching addresses containing {target_val}.")
    
    if match_count > 1000:
        print(f"Found {match_count} matches. That's too many to display! They are safely stored in Switch RAM.")
        print("Please change the score/combo in-game and run this script again to filter them down.")
        # Just create an empty matches_file to indicate we shouldn't reset next time
        with open(matches_file, "w") as f:
            f.write("MORE THAN 1000 MATCHES STORED IN C++ MOD\n")
    elif match_count > 0:
        matches = []
        array_size = match_count * 8
        array_data = read_absolute(s, addr_matches, array_size)
        
        with open(matches_file, "w") as f:
            for i in range(match_count):
                m = struct.unpack_from('<Q', array_data, i * 8)[0]
                matches.append(m)
                f.write(f"{m:X}\n")
                
        if match_count <= 20:
            print("Addresses:")
            for m in matches:
                offset = m - search_base
                print(f"  0x{m:X} (Offset from base region: 0x{offset:X})")
                
        print(f"\nSaved to {matches_file}.")
        print("To narrow it down, switch to a DIFFERENT value in the game,")
        print("then run this script again with the NEW value!")
    else:
        print("No matches found! Deleting matches.txt so you can start over.")
        if os.path.exists(matches_file):
            os.remove(matches_file)

    s.close()

if __name__ == "__main__":
    main()
