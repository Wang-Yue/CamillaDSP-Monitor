#!python3
import camilladsp
import sys
import time
import os
import curses
import glob
import subprocess
import select

cmd_l = ["./camilladsp", "spectrum_l.yml", "-p", "2345", '-l', 'warn', '-w']
cmd_r = ["./camilladsp", "spectrum_r.yml", "-p", "3456", '-l', 'warn', '-w']
proc_l = subprocess.Popen(cmd_l, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
proc_r = subprocess.Popen(cmd_r, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

c_l = camilladsp.CamillaConnection("127.0.0.1", 2345)
c_r = camilladsp.CamillaConnection("127.0.0.1", 3456)

stdscr = curses.initscr()
curses.noecho()
curses.cbreak()
stdscr.keypad(True)
stdscr.timeout(200)

def get_action():
  key = stdscr.getch()
  if key == ord('q'):
    curses.nocbreak()
    curses.echo()
    stdscr.keypad(False)
    curses.endwin()
    proc_l.terminate()
    proc_r.terminate()
    exit()

def print_spectrum_line(spectrum_l, spectrum_r, lineno):
  volume = -lineno * 5
  volume_string = "{:5.1f}   ".format(volume)
  for i, v in enumerate(spectrum_l):
    volume_string += "@" if (spectrum_l[i] > volume) else " "
    volume_string += "%" if (spectrum_r[i] > volume) else " "
  return volume_string

def print_output(msg, spectrum_l, spectrum_r):
  line = 0
  for lineno in range(13):
    spectrum_line = print_spectrum_line(spectrum_l, spectrum_r, lineno)
    stdscr.addstr(line, 0, spectrum_line)
    line += 1
  freq = "        25  40  63  100 157 250 430 630 1k  1k5 2k5 4k  6k3 10k 16k"
  stdscr.addstr(line, 0, freq)
  line += 1

  stdscr.move(line, 0)
  stdscr.addstr(line, 0, msg)
  stdscr.clrtoeol()
  stdscr.refresh()

retry_l = False
retry_r = False
while True:
  msg = ""
  spectrum_l = [-1000.0]*30
  spectrum_r = [-1000.0]*30

  try:
    if retry_l:
      c_l.connect()
    state = c_l.get_state()
    if state == camilladsp.ProcessingState.RUNNING:
      spectrum_l  = c_l.get_playback_signal_peak()
    if state == camilladsp.ProcessingState.STALLED:
      proc_l.terminate()
      c_l.exit()
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = c_l.get_stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        config = c_l.get_previous_config()
        config['devices']['capture_samplerate'] = int(reason.data)
        c_l.set_config(config)
        msg = "Successfully adjust spectrum to the new sample rate!"
    retry_l = False
  except ConnectionRefusedError as e:
    msg = "Can't connect to CamillaDSP, is it running? Error:" + str(e)
    retry_l = True
  except camilladsp.CamillaError as e:
    msg = "CamillaDSP replied with error:" + str(e)
    retry_l = True
  except IOError as e:
    msg = "Websocket is not connected:" + str(e)
    retry_l = True
  finally:
    pass # we ignore all errors for the spectrum display

  try:
    if retry_r:
      c_r.connect()
    state = c_r.get_state()
    if state == camilladsp.ProcessingState.RUNNING:
      spectrum_r  = c_r.get_playback_signal_peak()
    if state == camilladsp.ProcessingState.STALLED:
      proc_r.terminate()
      c_r.exit()
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = c_r.get_stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        config = c_r.get_previous_config()
        config['devices']['capture_samplerate'] = int(reason.data)
        c_r.set_config(config)
        msg = "Successfully adjust spectrum to the new sample rate!"
    retry_r = False
  except ConnectionRefusedError as e:
    msg = "Can't connect to CamillaDSP, is it running? Error:" + str(e)
    retry_r = True
  except camilladsp.CamillaError as e:
    msg = "CamillaDSP replied with error:" + str(e)
    retry_r = True
  except IOError as e:
    msg = "Websocket is not connected:" + str(e)
    retry_r = True
  finally:
    pass # we ignore all errors for the spectrum display

  get_action()
  print_output(msg, spectrum_l, spectrum_r)
