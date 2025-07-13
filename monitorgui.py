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
    # Start CamillaDSP subprocess for the main configuration window
    self.proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.3) # Give CamillaDSP a moment to start
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
      self.after(200, self.update) # Start the periodic update

  def destroy(self):
    # Ensure CamillaDSP subprocess is terminated on window close
    self.cdsp.general.exit()
    self.proc.terminate()
    Frame.destroy(self)

  def layout_monitor(self):
    # This list will store the Label widgets for dB values (e.g., -22.26 dB)
    self.vols = []
    
    # Create labels for each of the 8 monitor points (RMS/PEAK, Capture/Playback, Left/Right)
    i = 0
    monitor_points = [
        'RMS Capture Left', 'RMS Capture Right',
        'RMS Playback Left', 'RMS Playback Right',
        'PEAK Capture Left', 'PEAK Capture Right',
        'PEAK Playback Left', 'PEAK Playback Right'
    ]
    for label_text in monitor_points:
      lb = Label(self, text=label_text)
      # Place labels in column 0, starting from row 1
      lb.grid(column=0, row=i + 1, sticky="w", padx=5, pady=2) 
      i = i + 1

    # --- Canvas for custom progress bars ---
    # Define visual parameters for the canvas and the bars within it
    self.bar_canvas_width = 300 # Total width allocated for the bars within the canvas
    self.bar_height_monitor = 20 # Height of each individual horizontal bar
    self.bar_vertical_spacing = 5 # Vertical spacing between bars

    # Calculate total height required for the canvas to fit all 8 bars stacked vertically
    self.canvas_monitor_height = (self.bar_height_monitor + self.bar_vertical_spacing) * 8

    # Create the Canvas widget
    self.progress_canvas = Canvas(self, bg="lightgray", # Background color for the canvas area
                                  height=self.canvas_monitor_height,
                                  width=self.bar_canvas_width,
                                  highlightthickness=1, # Add a subtle border around the canvas
                                  highlightbackground="darkgray") # Color of the canvas border
    # Place the canvas in column 1, spanning 8 rows
    self.progress_canvas.grid(column=1, row=1, rowspan=8, sticky="nsew", padx=5, pady=2)

    # This list will store the IDs of the canvas rectangle items (our "progress bars")
    self.monitor_bars_canvas_ids = []

    for j in range(8):
      # Calculate initial coordinates for each horizontal bar within the canvas
      # x1 and x2 define the horizontal extent, y1 and y2 define the vertical extent.
      x1 = 0 # Bars start from the left edge of the canvas
      y1 = j * (self.bar_height_monitor + self.bar_vertical_spacing)
      x2 = 0 # Initially, the bar has zero width (x1=x2) to represent no progress
      y2 = y1 + self.bar_height_monitor

      # Create the rectangle item on the canvas
      bar_rect = self.progress_canvas.create_rectangle(
          x1, y1, x2, y2,
          fill="blue", outline="" # Default initial color, no outline for the bar itself
      )
      self.monitor_bars_canvas_ids.append(bar_rect)

      # Create and grid the dB value labels for each monitor point
      volume = '-1000' # Initial placeholder text for dB value
      vol = Label(self, text=volume)
      # Place dB labels in column 2, aligning with their respective rows
      vol.grid(column=2, row=j + 1, sticky="w", padx=5, pady=2)
      self.vols.append(vol)

  def layout_settings(self):
    def menu_item_selected(*args):
      # Update setting based on menu selection and reconfigure DSP
      for section, selected_setting in enumerate(selected_settings):
        self.setting[section] = selected_setting.get()
      self.setconfig()

    selected_settings = []
    self.mbs = [] # Menubuttons
    for i, section_name in enumerate(self.section_names):
      lb = Label(self, text=section_name)
      lb.grid(column=3, row=i + 1)
      mb = ttk.Menubutton(self, text=section_name, width=15)
      selected_settings.append(IntVar(value = self.setting[i]))
      selected_settings[i].trace_add('write', menu_item_selected) # Trigger update on change
      
      menu = Menu(mb, tearoff=0) # Create dropdown menu
      for j, subsection_name in enumerate(self.subsection_names[i]):
        menu.add_radiobutton(
            label=subsection_name, value=j, variable=selected_settings[i])
      mb.config(menu=menu)
      mb.grid(column=4, row=i + 1)
      self.mbs.append(mb)

  def layout_slider(self):
    def slider_changed(event):
      # Callback for volume slider changes
      new_volume = volume_value.get()
      self.cdsp.volume.set_main_volume(new_volume)

    def toggle_mute():
      # Callback for mute button
      self.cdsp.volume.set_main_mute(not self.cdsp.volume.main_mute())

    volume_value = DoubleVar() # Tkinter variable for slider value
    sl = Label(self, text='Volume:')
    sl.grid(column=0, row=0)
    
    self.volume_slider = Scale(
        self,
        from_=-100, # Min dB
        to=0,      # Max dB
        orient='horizontal',
        length=300,
        resolution=0.5, # Step size
        showvalue=False, # Don't show numeric value on the slider itself
        command=slider_changed, # Link to callback
        variable=volume_value) # Link to Tkinter variable
    self.volume_slider.grid(column=1, row=0)
    
    self.volume_label = Label(self, text='', width = 12) # Display actual volume dB
    self.volume_label.grid(column=2, row=0)
    
    self.samplerate_label = Label(self, text='Sample rate:') # Display sample rate
    self.samplerate_label.grid(column=3, row=0)
    
    self.mute_button = Button(
        self, text='Mute', width=5, relief='raised', command=toggle_mute) # Mute button
    self.mute_button.grid(column=4, row=0)

  def readconfig(self):
    # Read configuration settings from files and saved state
    self.setting = []
    self.section_names = []
    self.subsection_names = []
    for section in range(10):
      files = glob.glob(f'{str(section)}-*-*-*.yml')
      if len(files) != 0:
        samplefile = files[0]
        section_name = samplefile.split('-')[2]
        self.section_names.append(section_name)
        self.setting.append(0) # Default to first setting for each section
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

    # Load previously saved settings if available
    self.cached_stamp = os.stat(self.filename).st_mtime
    try:
      with open(self.dumpname, 'r') as file:
        setting = json.load(file)
        if type(setting) == type(self.setting):
          self.setting = setting
    except:
      pass # Ignore if file doesn't exist or is corrupted

  def setconfig(self):
    # Generate and apply the CamillaDSP configuration
    def generate_setting():
      s = open(self.filename, 'r').read() + 'pipeline:\n'
      for idx, val in enumerate(self.setting):
        files = glob.glob(f'{str(idx)}-{str(val)}-*')
        if len(files) != 1:
          raise Exception('Something is not right!')
        s += open(files[0], 'r').read()
      return s
    try:
      # Get current sample rate to preserve it
      config = self.cdsp.config.active()
      rate = config['devices']['samplerate']
      
      # Parse the new configuration from generated YAML string
      config = self.cdsp.config.parse_yaml(generate_setting())
      config['devices']['samplerate'] = rate # Reapply original sample rate
      self.cdsp.config.set_active(config) # Set the new active configuration
      print('Successfully updated DSP setting!')
    except camilladsp.CamillaError as e:
      print('Config has error: ', e)
    finally:
      # Update the text on Menubuttons to show current selection
      for i, mb in enumerate(self.mbs):
        mb.config(text=self.subsection_names[i][self.setting[i]])
    
    # Save current settings to a JSON file
    with open(self.dumpname, 'w') as file:
      json.dump(self.setting, file)

  def _map_monitor_amplitude_to_color(self, value, max_value=100):
      """
      Maps a scaled amplitude value (0-100) to an RGB hex color string.
      Color goes from green (low) to yellow (mid) to red (high).
      """
      norm_val = max(0, min(1, value / max_value)) # Normalize value to 0-1 range

      if norm_val <= 0.5:
          # Green to Yellow gradient for 0-50%
          # As norm_val goes from 0 to 0.5, red component increases from 0 to 255.
          # Green stays at 255, blue stays at 0.
          r = int(255 * (norm_val * 2))
          g = 255
          b = 0
      elif norm_val <= 0.75:
          # Yellow to Orange-Red gradient for 50-75%
          # As norm_val goes from 0.5 to 0.75, green component decreases.
          # Red stays at 255, blue stays at 0.
          factor = (norm_val - 0.5) / 0.25 # Scales 0.5-0.75 to 0-1
          r = 255
          g = int(255 * (1 - factor * 0.5)) # Green reduces to 50%
          b = 0
      else:
          # Orange-Red to Full Red gradient for 75-100%
          # As norm_val goes from 0.75 to 1, green component further decreases to 0.
          # Red stays at 255, blue stays at 0.
          factor = (norm_val - 0.75) / 0.25 # Scales 0.75-1 to 0-1
          r = 255
          g = int(255 * (0.5 - factor * 0.5)) # Green reduces from 50% to 0%
          b = 0
      return f"#{r:02x}{g:02x}{b:02x}" # Return hex color string

  def update(self):
    # This function is called periodically to update the GUI with current DSP levels
    state = self.cdsp.general.state()
    if state == camilladsp.ProcessingState.RUNNING:
      levels = self.cdsp.levels.levels()
      # Extract individual RMS and PEAK values for Left/Right channels
      values = [
          levels["capture_rms"][0], levels["capture_rms"][1],
          levels["playback_rms"][0], levels["playback_rms"][1],
          levels["capture_peak"][0], levels["capture_peak"][1],
          levels["playback_peak"][0], levels["playback_peak"][1]
      ]

      for i in range(8):
        current_db_value = values[i]
        # Scale the dB value from its range (e.g., -100 to 0 dB) to a 0-100 scale for our display
        # -100 dB maps to 0, 0 dB maps to 100.
        scaled_value = current_db_value + 100
        scaled_value = max(0, min(100, scaled_value)) # Clamp value between 0 and 100

        bar_id = self.monitor_bars_canvas_ids[i] # Get the canvas ID for the current bar
        
        # Get the current coordinates of the bar (x1, y1, x2, y2)
        # We only need y1 and y2 to preserve vertical position; x1 is always 0.
        # The x2 coordinate will be updated to reflect the progress.
        _, y1_current, _, y2_current = self.progress_canvas.coords(bar_id)
        
        # Calculate the new x2 coordinate based on the scaled_value (percentage progress)
        new_x2 = (scaled_value / 100.0) * self.bar_canvas_width

        # Update the bar's dimensions on the canvas
        self.progress_canvas.coords(bar_id, 0, y1_current, new_x2, y2_current)

        # Determine and apply the bar's color based on its scaled value
        color = self._map_monitor_amplitude_to_color(scaled_value, max_value=100)
        self.progress_canvas.itemconfig(bar_id, fill=color)

        # Update the dB value label next to the bar
        vol = self.vols[i]
        vol.config(text='{:8.2f} dB'.format(current_db_value))

      # Update main volume slider and label
      volume = self.cdsp.volume.main_volume()
      self.volume_label.configure(text='{:8.2f} dB'.format(volume))
      self.volume_slider.set(volume) # Update slider position
      
      # Update mute button text
      self.mute_button.config(
          text='Mute: On' if self.cdsp.volume.main_mute() else 'Mute: Off')
      
      # Update sample rate label
      sample_rate = str(self.cdsp.rate.capture())
      self.samplerate_label.config(text='Sample rate: ' + sample_rate)

      # Check for external configuration file changes
      stamp = os.stat(self.filename).st_mtime
      if stamp != self.cached_stamp:
        self.cached_stamp = stamp
        self.setconfig() # Reload config if file changed

    # Handle INACTIVE state (e.g., sample rate change)
    if state == camilladsp.ProcessingState.INACTIVE:
      reason = self.cdsp.general.stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        config = self.cdsp.config.previous()
        rate = int(reason.data)
        config['devices']['samplerate'] = rate
        self.cdsp.config.set_active(config)
        print('Successfully adjust to the new sample rate!')
    self.after(300, self.update) # Schedule the next update

class SpectrumAnalyzer(Frame): 
  def __init__(self, master=None, bar_height_max=150, **kwargs):
    super().__init__(master, **kwargs)
    self.master = master 
    cmd = ['./camilladsp', 'spectrum.yml', '-p', '5678', '-l', 'warn', '-w']
    # Start CamillaDSP subprocess for the spectrum analyzer
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
    
    # Calculate total canvas height (bars + text labels + padding)
    self.canvas_height = self.bar_max_height + self.text_area_height + 10 

    # Calculate total canvas width needed for all frequency bars
    total_bars_width = self.num_bars * (self.bar_width + self.bar_spacing) - self.bar_spacing
    self.canvas_width = total_bars_width

    # Create the Canvas for the spectrum analyzer display
    self.canvas = Canvas(self, bg="black", height=self.canvas_height,
                            width=self.canvas_width)
    self.canvas.pack(pady=10, padx=10) # Padding around the canvas within its frame

    self.bars = [] # List to store canvas rectangle IDs for spectrum bars
    self.text_labels = [] # List to store canvas text IDs for frequency labels

    # Create individual bars and frequency labels on the canvas
    for i in range(self.num_bars):
      x1 = i * (self.bar_width + self.bar_spacing)
      y1_bar_bottom = self.bar_max_height + 10 # Baseline for the bottom of the bars
      x2 = x1 + self.bar_width
      y2_bar_bottom = self.bar_max_height + 10

      # Create an empty rectangle (initially at baseline)
      bar_rect = self.canvas.create_rectangle(x1, y1_bar_bottom, x2, y2_bar_bottom, fill="green", outline="")
      self.bars.append(bar_rect)

      text_x = x1 + self.bar_width / 2 # Center text below the bar
      text_y = self.bar_max_height + self.text_area_height + 5 # Position of text label

      # Create frequency text label
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
      self.after(200, self.update) # Start periodic update for spectrum analyzer

  def destroy(self):
    # Terminate CamillaDSP subprocess on window close
    self.cdsp.general.exit()
    self.proc.terminate()
    Frame.destroy(self)

  def _map_amplitude_to_color(self, amplitude, max_amplitude=1.0):
    """
    Maps an amplitude value (0-1) to an RGB hex color string for the spectrum analyzer.
    Colors transition from blue (low) to green (mid) to red (high).
    """
    norm_amp = max(0, min(1, amplitude / max_amplitude))

    if norm_amp < 0.5:
      # Blue to Green transition
      r = 0
      g = int(255 * (norm_amp * 2)) # Green increases
      b = int(255 * (1 - norm_amp * 2)) # Blue decreases
    else:
      # Green to Red transition
      r = int(255 * ((norm_amp - 0.5) * 2)) # Red increases
      g = int(255 * (1 - (norm_amp - 0.5) * 2)) # Green decreases
      b = 0

    return f"#{r:02x}{g:02x}{b:02x}" # Return hex color string

  def update(self):
    # Periodically update the spectrum analyzer bars
    state = self.cdsp.general.state()
    if state == camilladsp.ProcessingState.RUNNING:
      # NOTE: This assumes self.cdsp.levels.playback_peak() returns a list
      # of values corresponding to the 30 frequency bands (e.g., 60 values for stereo bands).
      # If it only returns global L/R peaks, the spectrum visualization may not be accurate.
      peak = self.cdsp.levels.playback_peak()
      
      # Ensure `peak` has enough elements for processing all bands
      if len(peak) < self.num_bars * 2: 
        peak.extend([0.0] * (self.num_bars * 2 - len(peak))) # Pad with zeros if necessary

      spectrum = []
      # Process stereo pairs for each frequency band
      for i in range(0, min(len(peak), self.num_bars * 2), 2): 
        if i + 1 < len(peak): # Ensure a pair exists
          # Take the maximum of left and right channel for the band, then normalize
          val = max(peak[i], peak[i+1]) / 60.0 + 1.0 
          spectrum.append(val)
        else:
          spectrum.append(0.0) # Fallback for incomplete pairs

      for i, amp in enumerate(spectrum):
        if i >= self.num_bars: # Safety check to not exceed number of bars
            break
        bar_actual_height = amp * self.bar_max_height
        bar_actual_height = max(1, bar_actual_height) # Ensure bar has minimum height of 1 pixel

        # Get current coordinates of the bar
        x1, _, x2, _ = self.canvas.coords(self.bars[i])
        # Update only the y-coordinates to reflect the new height, drawing from the bottom up
        self.canvas.coords(self.bars[i], x1, (self.bar_max_height + 10) - bar_actual_height, x2, self.bar_max_height + 10)

        # Apply color based on the amplitude
        color = self._map_amplitude_to_color(amp, max_amplitude=1.0)
        self.canvas.itemconfig(self.bars[i], fill=color)

    if state == camilladsp.ProcessingState.INACTIVE:
      reason = self.cdsp.general.stop_reason()
      if reason == camilladsp.StopReason.CAPTUREFORMATCHANGE:
        sconfig = self.cdsp.config.previous()
        sconfig['devices']['capture_samplerate'] = int(reason.data)
        self.cdsp.config.set_active(sconfig)
        print('Successfully adjust spectrum to the new sample rate!')
    self.after(100, self.update) # Schedule the next update

if __name__ == '__main__':
  window = Tk()
  window.geometry('800x480') # Set initial window size
  
  s = ttk.Style()
  s.theme_use('default') # Use default Tkinter theme

  # Configure default font for better readability
  default_font = font.nametofont('TkDefaultFont')
  default_font.configure(size=7)
  window.option_add('*Font', default_font) # Apply to all widgets

  # Configure grid weights to make the window responsive
  # Both rows and the single column will expand/contract with the window
  window.grid_rowconfigure(0, weight=1) # Row for ConfigWindow
  window.grid_rowconfigure(1, weight=1) # Row for SpectrumAnalyzer
  window.grid_columnconfigure(0, weight=1) # Main content column

  # Create and place the ConfigWindow (top section)
  cw = ConfigWindow(window)
  cw.grid(column=0, row=0, sticky="nsew") # Make ConfigWindow fill its grid cell

  # Create and place the SpectrumAnalyzer (bottom section)
  sa = SpectrumAnalyzer(window)
  sa.grid(column=0, row=1, sticky="nsew") # Make SpectrumAnalyzer fill its grid cell

  window.mainloop()

