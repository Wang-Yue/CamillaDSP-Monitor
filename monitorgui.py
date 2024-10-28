#!python3
import camilladsp
import glob
import json
import os
import subprocess
import sys
import time
from tkinter import *
from tkinter import font
from tkinter import ttk

class ConfigWindow(Frame):

  def __init__(self, parent):
    Frame.__init__(self, parent)
    self.filename = 'setting.yml'
    self.dumpname = 'save.json'
    cmd = [
        './camilladsp', 'setting.yml', '-p', '1234', '-l', 'warn', '-w'
    ]
    self.proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.3)
    self.cdsp = camilladsp.CamillaClient('127.0.0.1', 1234)
    self.readconfig()
    self.layout_slider()
    self.layout_monitor()
    self.layout_settings()

    try:
      self.cdsp.connect()
      self.setconfig()
    except ConnectionRefusedError as e:
      print("Can't connect to CamillaDSP, is it running? Error:" + str(e))
    except camilladsp.CamillaError as e:
      print('CamillaDSP replied with error:' + str(e))
    except IOError as e:
      print('Websocket is not connected:' + str(e))
    finally:
      self.after(200, self.update)

  def destroy(self):
    self.cdsp.general.exit()
    self.proc.terminate()
    Frame.destroy(self)

  def layout_monitor(self):
    self.pbs = []
    self.vols = []
    i = 0
    for volume_string in ['RMS', 'PEAK']:
      for source_string in ['Capture', 'Playback']:
        for channel_string in ['Left', 'Right']:
          label = volume_string + ' ' + source_string + ' ' + channel_string
          lb = Label(self, text=label)
          lb.grid(column=0, row=i + 1)
          i = i + 1
    for i in range(8):
      pb = ttk.Progressbar(
          self,
          orient='horizontal',
          mode='determinate',
          length=400,
          maximum=100,
          style='TProgressbar')
      pb.grid(column=1, row=i + 1)
      self.pbs.append(pb)
      volume = '-1000'
      vol = Label(self, text=volume)
      vol.grid(column=2, row=i + 1)
      self.vols.append(vol)

  def layout_settings(self):
    def menu_item_selected(*args):
      for section, selected_setting in enumerate(selected_settings):
        self.setting[section] = selected_setting.get()
      self.setconfig()
    selected_settings = []
    self.mbs = []
    for i, section_name in enumerate(self.section_names):
      lb = Label(self, text=section_name)
      lb.grid(column=3, row=i + 1)
      mb = ttk.Menubutton(self, text=section_name, width=30)
      selected_settings.append(IntVar(value = self.setting[i]))
      selected_settings[i].trace('w', menu_item_selected)
      menu = Menu(mb, tearoff=0)
      for j, subsection_name in enumerate(self.subsection_names[i]):
        menu.add_radiobutton(
            label=subsection_name, value=j, variable=selected_settings[i])
      mb.config(menu=menu)
      mb.grid(column=4, row=i + 1)
      self.mbs.append(mb)

  def layout_slider(self):
    def slider_changed(event):
      new_volume = volume_value.get()
      self.cdsp.volume.set_main_volume(new_volume)
    def toggle_mute():
      self.cdsp.volume.set_main_mute(not self.cdsp.volume.main_mute())
    volume_value = DoubleVar()
    sl = Label(self, text='Volume:')
    sl.grid(column=0, row=0)
    self.volume_slider = Scale(
        self,
        from_=-100,
        to=0,
        orient='horizontal',
        length=400,
        resolution=0.5,
        showvalue=False,
        command=slider_changed,
        variable=volume_value)
    self.volume_slider.grid(column=1, row=0)
    self.volume_label = Label(self, text='', width = 12)
    self.volume_label.grid(column=2, row=0)
    self.samplerate_label = Label(self, text='Sample rate:')
    self.samplerate_label.grid(column=3, row=0)
    self.mute_button = Button(
        self, text='Mute', width=20, relief='raised', command=toggle_mute)
    self.mute_button.grid(column=4, row=0)

  def readconfig(self):
    self.setting = []
    self.section_names = []
    self.subsection_names = []
    for section in range(10):
      files = glob.glob(f'{str(section)}-*-*-*.yml')
      if len(files) != 0:
        samplefile = files[0]
        section_name = samplefile.split('-')[2]
        self.section_names.append(section_name)
        self.setting.append(0)
        subsections = []
        for subsection in range(10):
          subfiles = glob.glob(f'{str(section)}-{str(subsection)}-*-*.yml')
          if len(subfiles) == 1:
            subsection_name = subfiles[0].split('-')[3]
            subsection_name = subsection_name.split('.')[0]
            subsections.append(subsection_name)
          else:
            break
        self.subsection_names.append(subsections)

    self.cached_stamp = os.stat(self.filename).st_mtime
    try:
      with open(self.dumpname, 'r') as file:
        setting = json.load(file)
        if type(setting) == type(self.setting):
          self.setting = setting
    except:
      pass

  def setconfig(self):
    def generate_setting():
      s = open(self.filename, 'r').read() + 'pipeline:\n'
      for idx, val in enumerate(self.setting):
        files = glob.glob(f'{str(idx)}-{str(val)}-*')
        if len(files) != 1:
          raise Exception('Something is not right!')
        s += open(files[0], 'r').read()
      return s
    try:
      # Get current rate.
      config = self.cdsp.config.active()
      rate = config['devices']['samplerate']
      # Get updated config.
      config = self.cdsp.config.parse_yaml(generate_setting())
      config['devices']['samplerate'] = rate
      self.cdsp.config.set_active(config)
      print('Successfully updated DSP setting!')
    except camilladsp.CamillaError as e:
      print('Config has error: ', e)
    finally:
      for i, mb in enumerate(self.mbs):
        mb.config(text=self.subsection_names[i][self.setting[i]])
    with open(self.dumpname, 'w') as file:
      json.dump(self.setting, file)

  def update(self):
    state = self.cdsp.general.state()
    if state == camilladsp.ProcessingState.RUNNING:
      levels = self.cdsp.levels.levels()
      values = [
          levels["capture_rms"],
          levels["playback_rms"],
          levels["capture_peak"],
          levels["playback_peak"]
      ]
      values = sum(values, [])
      for i in range(8):
        value = values[i] + 100
        if value < 0: value = 0
        if value > 100: value = 100
        pb = self.pbs[i]
        pb.config(value=value)
        vol = self.vols[i]
        vol.config(text='{:8.2f} dB'.format(values[i]))
      volume = self.cdsp.volume.main_volume()
      self.volume_label.configure(text='{:8.2f} dB'.format(volume))
      self.volume_slider.set(volume)
      self.mute_button.config(
          text='Mute: On' if self.cdsp.volume.main_mute() else 'Mute: Off')
      sample_rate = str(self.cdsp.rate.capture())
      self.samplerate_label.config(text='Sample rate: ' + sample_rate)

      stamp = os.stat(self.filename).st_mtime
      if stamp != self.cached_stamp:
        self.cached_stamp = stamp
        self.setconfig()

    if state == camilladsp.ProcessingState.INACTIVE:
      reason = self.cdsp.general.stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        config = self.cdsp.config.previous()
        rate = int(reason.data)
        config['devices']['samplerate'] = rate
        self.cdsp.config.set_active(config)
        print('Successfully adjust to the new sample rate!')
    self.after(200, self.update)


class SpectrumAnalyser(Frame):

  def __init__(self, parent):
    Frame.__init__(self, parent)
    cmd = ['./camilladsp', 'spectrum.yml', '-p', '5678', '-l', 'warn', '-w']
    self.proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.3)
    self.cdsp = camilladsp.CamillaClient('127.0.0.1', 5678)
    self.pbs = []
    freq = [
        '25', '31.5', '40', '50', '63', '80', '100', '125', '160', '200', '250',
        '315', '400', '500', '630', '800', '1k', '1k25', '1k6', '2k', '2k5',
        '3k15', '4k', '5k', '6k3', '8k', '10k', '12k5', '16k', '20k'
    ]
    for i in range(30):
      pb = ttk.Progressbar(
          self,
          orient='vertical',
          mode='determinate',
          length=400,
          maximum=60,
          style='TProgressbar')
      pb.grid(column=i, row=0, sticky='w')
      self.pbs.append(pb)
      frequency = freq[i]
      lb = Label(self, text=frequency, font=(None, 14))
      lb.grid(column=i, row=1)
    try:
      self.cdsp.connect()
    except ConnectionRefusedError as e:
      print("Can't connect to CamillaDSP, is it running? Error:" + str(e))
    except camilladsp.CamillaError as e:
      print('CamillaDSP replied with error:' + str(e))
    except IOError as e:
      print('Websocket is not connected:' + str(e))
    finally:
      self.after(200, self.update)

  def destroy(self):
    self.cdsp.general.exit()
    self.proc.terminate()
    Frame.destroy(self)

  def update(self):
    state = self.cdsp.general.state()
    if state == camilladsp.ProcessingState.RUNNING:
      spectrum = self.cdsp.levels.playback_peak()
      for i in range(30):
        value = max(spectrum[i * 2], spectrum[i * 2 + 1]) + 60
        if value < 0: value = 0
        if value > 60: value = 60
        pb = self.pbs[i]
        pb.config(value=value)
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = self.cdsp.general.stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        sconfig = self.cdsp.config.previous()
        sconfig['devices']['capture_samplerate'] = int(reason.data)
        self.cdsp.config.set_active(sconfig)
        print('Successfully adjust spectrum to the new sample rate!')
    self.after(200, self.update)


if __name__ == '__main__':
  window = Tk()
  window.geometry('1400x900')
  s = ttk.Style()
  s.theme_use('default')
  s.configure('TProgressbar', thickness=40)
  default_font = font.nametofont('TkDefaultFont')
  default_font.configure(size=20)
  window.option_add('TkDefaultFont', default_font)

  sa = SpectrumAnalyser(window)
  sa.grid(column=0, row=1)
  cw = ConfigWindow(window)
  cw.grid(column=0, row=0)

  window.grid_rowconfigure(0, weight=1)
  window.grid_columnconfigure(0, weight=1)

  window.mainloop()
