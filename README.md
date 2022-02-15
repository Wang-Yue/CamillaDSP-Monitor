# CamillaDSP Monitor

A host monitor program for CamillaDSP.

# Introduction

The [CamillaDSP](https://github.com/HEnquist/camilladsp/) project provides a seamless approach to do DSP system-wide on all major desktop operating systems, with various DSP features ready to be used, such as [Loudness](https://en.wikipedia.org/wiki/Loudness_compensation), [Convolution](https://en.wikipedia.org/wiki/Convolution), or [Parametric EQ](https://en.wikipedia.org/wiki/Equalization_(audio)#Parametric_equalizer).  The program utilizes a setting file that can be sent to the [websocket](https://henquist.github.io/0.6.3/websocket.html) in run time. 

This program provides an easy interface to access various useful DSP features. Users can enable/disable complex DSP functionalities with just one or two key presses. It also displays a very useful spectrum analyzer in the terminal.

# DSPs available

The DSP workflow is heavily inspired by the [RME ADI-2 DAC](https://www.rme-audio.de/adi-2-dac.html). It offers the following functionalities, and more can be added in the same way.

## Volume Control

Current volume setting is displayed on top of the screen.

## Status Monitor

The current sample rate, RMS and peak volume of both input and output channels are clearly displayed on the terminal

## Mute

The mute function will mute the audio output. Its status is shown right after the volume setting.

## Width

Define stereo width. Mono for mono, and Swapped have the channels swapped.

## M/S Proc

Activates M/S processing. Monaural content is sent to the left, stereo to the right channel.

## Phase Inversion

Some poorly mastered recordings have inverted phase and the script can correct it.

## Crossfeed

The script offers 5 levels of headphone crossfeed based on my [CamillaDSP-Crossfeed](https://github.com/Wang-Yue/camilladsp-crossfeed/) project.

## EQ

The script provides EQ function for headphone and room correction.

## Loudness

The script offers the same Loudness feature as the RME ADI-2 DAC.

## Emphasis

In the early times of digital audio, pre- and de-emphasis were used for radio transmission. The audio signal is equalized to have treble boosted when recorded. When played back an analog treble filter is required. Many older CDs were recorded with Emphasis.

DAC chips, modern ones included, usually hava a register to turn on de-emphasis but not many audio products use this feature. As a result, old CDs may feel too bright when playing via modern hardware. This project offers an identical solution implemented in software. User may choose to pre-emphasis or de-emphasis the signal before sending it to the output.

## DC Protection

With latest RME ADI-2 DAC/Pro firmware, a DC Protection filter is added to remove potentially harmful DC in the digital source signal. This project provides an identical implementation.

This filter adds a special, smooth high pass to the DA path, with zero latency, very low THD and phase deviation. This filter has a corner frequency of 7 Hz, to not only cancel DC but also reduce the amount of inaudible and imperceptible infrasound a bit that plagues some sources.

## Spectrum analyzer

The Analyzer is modeled after RME’s ADI-2 DAC, which is based on Spectral Analyzer in DIGICheck. 

It uses 29 biquad bandpass filters for high separation between the bands, providing outstanding musical visualization.
To be able to also show DC content the lowest band is not a band-pass filter, but a low pass, catching the whole range
from 0 Hz up to 30 Hz. With some unusual signals it therefore can happen that the level shown will be a bit higher than expected.

The shown frequency range is always the human audible range, 20 Hz up to 20 kHz.

As opposed to most other solutions no FFT (Fast Fourier Transform) is used. The Spectral
Analyzer performs a true band-pass filter calculation, as in professional hardware devices. The
frequency distance between the filters is scaled matching human hearing. 

The Spectrum Analyzer shows spectrum for both left and right channels. `%` symbol indicates level for the left channel and `@` for the right channel.

# User Interface

The script has a simple terminal user interface. When being executed, CamillaDSP is started automatically.

![Screenshot](screenshot.png)

## Prerequisites

The program is written in Python 3. It also uses the [PyCamillaDSP](https://github.com/HEnquist/pycamilladsp/) library.
So please install both before running this program.

## Setup

You need to edit the device section in `setting.yml` and `spectrum.yml` to use your loopback and playback devices.

## Startup

Simply by invoking `./monitor.py` in command line.

## Control

You can control the output volume by arrow keys. `LEFT` and `RIGHT` adjust volume by 10dB, while `UP` and `DOWN` adjust it by 1dB.

You can mute or unmute by pressing the `m` key. The mute status will be displayed right after the volume setting.

You can enable/disable DSP features by number keys. For instance press `31` turn on the loudness feature, and `30` to turn it off.

You can quit the program and CamillaDSP by pressing `q`. 

## Parameter Tuning

You can open a text editor and tune parameters in `setting.yml`. The script automatically checks if the file is changed, and will reflect your changes immediately in the DSP. 

## Automatic Sample Rate Switching

The script will automatically restart the DSP program when it pauses due to sample rate changes.

# Special Thanks

Many thanks to @HEnquist for the wonderful CamillaDSP program, and to RME for creating the mighty ADI-2 DAC that inspred this script.

# FAQ

Q: There's already an official CamillaDSP project, [CamillaGUI](https://github.com/HEnquist/camillagui), which does the similar thing. Why are you reinventing the wheel?

A: CamillaGUI allows run time edit of the parameters and pipeline, and greatly simplifies the workflow for various tasks. Unfortunately, there're certain drawbacks of these programs, mainly in the following perspectives:

1. The GUI program has no way to disable/enable a set of filters/mixers in runtime. Some EQ functions, such as room correction or headphone EQ, require a set filters to be enabled or disabled at the same time. Other functions, such as headphone crossfeed, needs a few filters in combination with a few mixers to work together. Neither GUI nor the setting file has the concept of "functional blocks" that allows a group of filters/mixers to work together.

2. The changes to the parameters are not reflected in the DSP immediately. One has to save and apply the settings so see its effect. This is a cumbersome process.

3. There's no automatic error handling in the GUI program. For instance, when the sample rate of the input device changes, CamillaDSP will pause and need another control program to correct itself. the GUI program does not provide this feature.

That's why I build this short script to address the above issues. It also has a very nice spectrum analyzer built in. However, the script is by no means a mature software product compared to CamillaGUI. It's just for me to access to some DSP workflow I usually performs. It's super hacky and may crash when you're holding it wrong.

Q: I run into an error: `_curses.error: addwstr() returned ERR` 

A: The program occupies some terminal space. Make your terminal window big enough.

Q: I run into an error: `rate = config['devices']['samplerate']  TypeError: 'NoneType' object is not subscriptable`

A: Make sure you edited the device section in both `settings.yml` and `spectrum.yml` to match your loopback and playback devices. Double check your devices support the the format and sample rate you specified. Also make sure there's no other CamillaDSP process running.

Q: I run into an error: `raise AttributeError(name) from None AttributeError: STALLED`

You need to use the most recent version of `pycamilladsp` (in `next` branch). `STALLED` was added in this [commit](https://github.com/HEnquist/pycamilladsp/commit/1ec0fb4bc7a056dff1b07c2d46ce36db3993b6eb).

