#!python3
import camilladsp
import sys
import time
import os
import curses
import glob
import subprocess
import select

cmd = ["./camilladsp", "setting.yml", "-p", "1234", '-g-20', '-l', 'warn', '-w']
scmd = ["./camilladsp", "spectrum.yml", "-p", "5678", '-l', 'warn', '-w']
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
sproc = subprocess.Popen(scmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

c = camilladsp.CamillaConnection("127.0.0.1", 1234)
sc = camilladsp.CamillaConnection("127.0.0.1", 5678)

stdscr = curses.initscr()
curses.noecho()
curses.cbreak()
stdscr.keypad(True)
stdscr.timeout(200)

setting = []
setting_count = []
section_names = []
subsection_names = []
for section in range(10):
  files = glob.glob(f"{str(section)}-*-*-*.yml")
  if len(files) != 0:
    samplefile = files[0]
    section_name = samplefile.split('-')[2]
    section_names.append(section_name)
    setting_count.append(0)
    setting.append(0)
    subsections = []
    for subsection in range (10):
      subfiles = glob.glob(f"{str(section)}-{str(subsection)}-*-*.yml")
      if len(subfiles) == 1:
        subsection_name = subfiles[0].split('-')[3]
        subsection_name = subsection_name.split('.')[0]
        subsections.append(subsection_name)
        setting_count[-1] += 1
      else:
        break
    subsection_names.append(subsections) 

filename = 'setting.yml'
cached_stamp = os.stat(filename).st_mtime

def generate_setting():
  s = open(filename, 'r').read()
  s += 'pipeline:\n'
  for idx, val in enumerate(setting):
    files = glob.glob(f'{str(idx)}-{str(val)}-*')
    if len(files) != 1:
      raise Exception('Something is not right!')
    s += open(files[0], 'r').read()
  return s

prev_key = ''
def get_action():
  global prev_key
  key = stdscr.getch()
  if key == ord('q'):
    curses.nocbreak()
    curses.echo()
    stdscr.keypad(False)
    curses.endwin()
    proc.terminate()
    sproc.terminate()
    exit()
  elif key == curses.KEY_UP: 
    return 1
  elif key == curses.KEY_RIGHT: 
    return 10
  elif key == curses.KEY_DOWN: 
    return -1
  elif key == curses.KEY_LEFT: 
    return -10
  elif key == ord('m'):
    return 'm'
  elif key >= ord('0') and key <= ord('9'):
    if prev_key == '':
      prev_key = key
    else:
      section = prev_key - ord('0')
      prev_key = ''
      subsection = key - ord('0')
      if section < len(setting_count) and subsection < setting_count[section]:
        setting[section] = subsection
      return 0
  return None

def volume_string(volume): 
  length = 30
  range = 100
  pieces = int(-volume * length / range)
  if pieces < 0:
    pieces = 0
  if pieces > length:
    pieces = length 
  blocks = '='*(length - pieces)
  spaces = ' '*pieces
  volume_string = "{:8.2f}".format(volume)
  return f"[{blocks}{spaces}] {volume_string}dB"
 
def print_spectrum_line(spectrum, lineno):
  volume = -lineno * 5
  volume_string = "{:5.1f}   ".format(volume)
  for i, v in enumerate(spectrum):
    volume_string += ("%" if i %2 == 0 else "@") if (v > volume) else " "
  return volume_string

def print_output(msg, vol, rate, values, spectrum, mute):
  volume = ["RMS ", "PEAK"]
  source = ["Capture ", "Playback"]
  channel = ["Left  ", "Right "]
  mute_string = "[m]" if mute else "[ ]"
  stdscr.addstr(0, 0, f"Volume {volume_string(vol)} {mute_string}  Rate: {rate}\n")
  line = 1
  for v in volume:
    for s in source:
      for c in channel:
        vol_bar = volume_string(values[line - 1])
        stdscr.addstr(line, 0, f"{v} {s} {c} {vol_bar}\n")
        line += 1
  for idx, section_name in enumerate(section_names):
    s = f"{idx}. {section_name}"
    for j, subsection_name in enumerate(subsection_names[idx]):
      checked = '  [x] ' if j == setting[idx] else '  [ ] '
      s += checked + subsection_name
    stdscr.addstr(line, 0, f" {s}\n")
    line += 1

  for lineno in range(13):
    spectrum_line = print_spectrum_line(spectrum, lineno)
    stdscr.addstr(line, 0, spectrum_line)
    line += 1
  freq = "        25  40  63  100 157 250 430 630 1k  1k5 2k5 4k  6k3 10k 16k"
  stdscr.addstr(line, 0, freq)
  line += 1

  stdscr.move(line, 0)
  if msg == '' and  select.select([proc.stdout],[],[],0.0)[0]:
    msg = proc.stdout.readline()
  msg = msg.strip()[0:70]
  stdscr.addstr(line, 0, msg)
  stdscr.clrtoeol()
  stdscr.refresh()

def setconfig():
  try:
    # Get current rate.
    config = c.get_config()
    rate = config['devices']['samplerate']
    # Get updated config.
    config = c.read_config(generate_setting())
    config['devices']['samplerate'] = rate
    c.set_config(config)
    return "Successfully updated DSP setting!"
  except camilladsp.CamillaError as e:
    return "Config has error!"
 
retry = False
sretry = False
while True:
  msg = ""
  volume = 0.0
  sample_rate = 0.0
  mute = False
  values = [-1000.0]*8
  spectrum = [-1000.0]*30
  action = get_action()

  try:
    if sretry:
      sc.connect()
    sstate = sc.get_state()
    if sstate == camilladsp.ProcessingState.RUNNING:
      spectrum  = sc.get_playback_signal_peak()
    if sstate == camilladsp.ProcessingState.STALLED:
      sproc.terminate()
      sc.exit()
    if sstate == camilladsp.ProcessingState.INACTIVE:
      reason = sc.get_stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        sconfig = sc.get_previous_config()
        sconfig['devices']['capture_samplerate'] = int(reason.data)
        sc.set_config(sconfig)
        msg = "Successfully adjust spectrum to the new sample rate!"
    sretry = False
  except ConnectionRefusedError as e:
    msg = "Can't connect to CamillaDSP, is it running? Error:" + str(e)
    sretry = True
  except camilladsp.CamillaError as e:
    msg = "CamillaDSP replied with error:" + str(e)
    sretry = True
  except IOError as e:
    msg = "Websocket is not connected:" + str(e)
    sretry = True
  finally:
    pass # we ignore all errors for the spectrum display

  try:
    if retry:
      c.connect()
      msg = setconfig()
    volume = c.get_volume()
    mute = c.get_mute()
    sample_rate = c.get_capture_rate()
    state = c.get_state()
    if state == camilladsp.ProcessingState.RUNNING:
      values = [c.get_capture_signal_rms(), c.get_playback_signal_rms(), c.get_capture_signal_peak(), c.get_playback_signal_peak()]
      values = sum(values, [])
      stamp = os.stat(filename).st_mtime
      if stamp != cached_stamp or action == 0:
        cached_stamp = stamp
        msg = setconfig()
    if state == camilladsp.ProcessingState.STALLED:
      proc.terminate()
      c.exit()
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = c.get_stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        config = c.get_previous_config()
        rate = int(reason.data)
        config['devices']['samplerate'] = rate
        c.set_config(config)
        msg = "Successfully adjust to the new sample rate!"
    retry = False

    if action != 0 and action != None:
      if action == 'm':
        c.set_mute(not mute)
      else:
        new_volume = action + volume
        c.set_volume(new_volume)
        volume = c.get_volume()

  except ConnectionRefusedError as e:
    msg = "Can't connect to CamillaDSP, is it running? Error:" + str(e)
    retry = True
  except camilladsp.CamillaError as e:
    msg = "CamillaDSP replied with error:" + str(e)
    retry = True
  except IOError as e:
    msg = "Websocket is not connected:" + str(e)
    retry = True
  finally:
    print_output(msg, volume, sample_rate, values, spectrum, mute)
