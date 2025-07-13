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
          length=130,
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
      mb = ttk.Menubutton(self, text=section_name, width=15)
      selected_settings.append(IntVar(value = self.setting[i]))
      selected_settings[i].trace_add('write', menu_item_selected)
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
        length=130,
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
        self, text='Mute', width=5, relief='raised', command=toggle_mute)
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
    self.after(1000, self.update)


class SpectrumAnalyzer(Frame): # Changed to inherit from tk.Frame
  def __init__(self, master=None, bar_height_max=150, **kwargs):
    # Pass master and other keyword arguments to the Frame constructor
    super().__init__(master, **kwargs)
    self.master = master # Store master for potential later use if needed
    cmd = ['./camilladsp', 'spectrum.yml', '-p', '5678', '-l', 'warn', '-w']
    self.proc = subprocess.Popen(
      cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.3)
    self.cdsp = camilladsp.CamillaClient('127.0.0.1', 5678)
    self.freq = [
      '25', '31.5', '40', '50', '63', '80', '100', '125', '160', '200', '250',
      '315', '400', '500', '630', '800', '1k', '1k25', '1k6', '2k', '2k5',
      '3k15', '4k', '5k', '6k3', '8k', '10k', '12k5', '16k', '20k'
    ]
    self.num_bars = len(self.freq)
    self.bar_width = 20
    self.bar_spacing = 5

    self.bar_max_height = bar_height_max
        
    self.text_area_height = 30
    self.text_font_size = 7
    
    # The total height of the canvas must accommodate bars AND text
    self.canvas_height = self.bar_max_height + self.text_area_height + 10 # 10px padding at top of canvas

    # Calculate total width needed for bars and spacing
    total_bars_width = self.num_bars * (self.bar_width + self.bar_spacing) - self.bar_spacing
    
    # The padding for the canvas should now be handled internally by the frame's packing
    # Or, passed as a parameter to the frame if it dictates the frame's size.
    # For simplicity, we'll size the canvas directly and pack the frame.
    self.canvas_width = total_bars_width


    # Canvas is now packed directly into this Frame instance
    self.canvas = Canvas(self, bg="black", height=self.canvas_height,
                            width=self.canvas_width)
    # Use .pack() directly on the Frame, as it's now the main container for the canvas
    self.canvas.pack(pady=10, padx=10) # Some internal padding within the frame

    self.bars = []
    self.text_labels = []

    for i in range(self.num_bars):
      x1 = i * (self.bar_width + self.bar_spacing)
      y1_bar_bottom = self.bar_max_height + 10 # Baseline for bars within canvas
      x2 = x1 + self.bar_width
      y2_bar_bottom = self.bar_max_height + 10

      bar_rect = self.canvas.create_rectangle(x1, y1_bar_bottom, x2, y2_bar_bottom, fill="green", outline="")
      self.bars.append(bar_rect)

      text_x = x1 + self.bar_width / 2
      text_y = self.bar_max_height + self.text_area_height + 5 # Position within canvas

      label = self.canvas.create_text(text_x, text_y, text=self.freq[i], fill="white",
                                      font=("Helvetica", self.text_font_size))
      self.text_labels.append(label)

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

  def _map_amplitude_to_color(self, amplitude, max_amplitude=1.0):
    norm_amp = max(0, min(1, amplitude / max_amplitude))

    if norm_amp < 0.5:
      r = 0
      g = int(255 * (norm_amp * 2))
      b = int(255 * (1 - norm_amp * 2))
    else:
      r = int(255 * ((norm_amp - 0.5) * 2))
      g = int(255 * (1 - (norm_amp - 0.5) * 2))
      b = 0

    return f"#{r:02x}{g:02x}{b:02x}"


  def update(self):
    state = self.cdsp.general.state()
    if state == camilladsp.ProcessingState.RUNNING:
      peak = self.cdsp.levels.playback_peak()
      spectrum = [max(peak[i], peak[i+1]) / 60.0 + 1.0 for i in range(0, len(peak), 2)]
      for i, amp in enumerate(spectrum):
        bar_actual_height = amp * self.bar_max_height
        bar_actual_height = max(1, bar_actual_height)

        x1, _, x2, _ = self.canvas.coords(self.bars[i])
        self.canvas.coords(self.bars[i], x1, (self.bar_max_height + 10) - bar_actual_height, x2, self.bar_max_height + 10)

        color = self._map_amplitude_to_color(amp, max_amplitude=1.0)
        self.canvas.itemconfig(self.bars[i], fill=color)
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = self.cdsp.general.stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        sconfig = self.cdsp.config.previous()
        sconfig['devices']['capture_samplerate'] = int(reason.data)
        self.cdsp.config.set_active(sconfig)
        print('Successfully adjust spectrum to the new sample rate!')
    self.after(100, self.update)

if __name__ == '__main__':
  window = Tk()
  window.geometry('800x480')
  s = ttk.Style()
  s.theme_use('default')
  s.configure('TProgressbar', thickness=11)
  default_font = font.nametofont('TkDefaultFont')
  default_font.configure(size=7)
  window.option_add('TkDefaultFont', default_font)

  sa = SpectrumAnalyzer(window)
  sa.grid(column=0, row=1)
  cw = ConfigWindow(window)
  cw.grid(column=0, row=0)

  window.grid_rowconfigure(0, weight=1)
  window.grid_columnconfigure(0, weight=1)

  window.mainloop()
