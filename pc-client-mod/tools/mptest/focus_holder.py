# Test aid: a small window on DISPLAY2 that takes the foreground FROM THE GAME
# (only if the game is foreground) so MP tests run unfocused. Sends no input.
import tkinter as tk, sys, ctypes, time
u=ctypes.windll.user32; k=ctypes.windll.kernel32
r=tk.Tk(); r.title("MP test focus holder"); r.geometry("360x50+1950+1140")
tk.Label(r,text="MP test: keeping the game unfocused").pack(expand=True)
def grab():
    me=int(r.frame(),16)
    fg=u.GetForegroundWindow()
    a=u.GetWindowThreadProcessId(fg,None); b=k.GetCurrentThreadId()
    u.AttachThreadInput(b,a,True); u.SetForegroundWindow(me); u.AttachThreadInput(b,a,False)
r.after(500,grab); r.after(int(sys.argv[1])*1000 if len(sys.argv)>1 else 1800000,r.destroy)
r.mainloop()
