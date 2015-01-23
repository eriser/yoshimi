/*
    SynthEngine.cpp

    Original ZynAddSubFX author Nasca Octavian Paul
    Copyright (C) 2002-2005 Nasca Octavian Paul
    Copyright 2009-2011, Alan Calvert
    Copyright 2009, James Morris
    Copyright 2014, Will Godfrey & others

    This file is part of yoshimi, which is free software: you can redistribute
    it and/or modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either version 2 of
    the License, or (at your option) any later version.

    yoshimi is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.   See the GNU General Public License (version 2 or
    later) for more details.

    You should have received a copy of the GNU General Public License along with
    yoshimi; if not, write to the Free Software Foundation, Inc., 51 Franklin
    Street, Fifth Floor, Boston, MA  02110-1301, USA.

    This file is derivative of original ZynAddSubFX code, last modified January 2015
*/

#include<stdio.h>
#include <sys/time.h>

using namespace std;

#include "MasterUI.h"
#include "Misc/SynthEngine.h"
#include "Misc/Config.h"

#include <iostream>

static unsigned int synthNextId = 0;

SynthEngine::SynthEngine(int argc, char **argv, bool _isLV2Plugin) :
    uniqueId(synthNextId++),
    isLV2Plugin(_isLV2Plugin),
    bank(this),
    Runtime(this, argc, argv),
    presetsstore(this),
    shutup(false),
    samplerate(48000),
    samplerate_f(samplerate),
    halfsamplerate_f(samplerate / 2),
    buffersize(256),
    buffersize_f(buffersize),
    oscilsize(1024),
    oscilsize_f(oscilsize),
    halfoscilsize(oscilsize / 2),
    halfoscilsize_f(halfoscilsize),
    processOffset(0),
    p_buffersize(0),
    p_bufferbytes(0),
    p_buffersize_f(0),
    ctl(NULL),
    microtonal(this),    
    fft(NULL),    
    muted(0xFF),
    tmpmixl(NULL),
    tmpmixr(NULL),
    processLock(NULL),
    vuringbuf(NULL),
    stateXMLtree(NULL),
    guiMaster(NULL),
    guiClosedCallback(NULL),
    guiCallbackArg(NULL),
    LFOtime(0)
{    

    memset(&random_state, 0, sizeof(random_state));

    ctl = new Controller(this);
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        part[npart] = NULL;
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        insefx[nefx] = NULL;
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
        sysefx[nefx] = NULL;
    shutup = false;
}


SynthEngine::~SynthEngine()
{
    closeGui();
    if (vuringbuf)
        jack_ringbuffer_free(vuringbuf);
    
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        if (part[npart])
            delete part[npart];

    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        if (insefx[nefx])
            delete insefx[nefx];
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
        if (sysefx[nefx])
            delete sysefx[nefx];

    if (tmpmixl)
        fftwf_free(tmpmixl);
    if (tmpmixr)
        fftwf_free(tmpmixr);
    if (fft)
        delete fft;
    pthread_mutex_destroy(&processMutex);
    if (ctl)
        delete ctl;
}


bool SynthEngine::Init(unsigned int audiosrate, int audiobufsize)
{
    samplerate_f = samplerate = audiosrate;
    halfsamplerate_f = samplerate / 2;
    buffersize_f = buffersize = audiobufsize;
    bufferbytes = buffersize * sizeof(float);
    oscilsize_f = oscilsize = Runtime.Oscilsize;
    halfoscilsize_f = halfoscilsize = oscilsize / 2;

    if (!pthread_mutex_init(&processMutex, NULL))
        processLock = &processMutex;
    else
    {
        Runtime.Log("SynthEngine actionLock init fails :-(");
        processLock = NULL;
        goto bail_out;
    }


    memset(random_state, 0, sizeof(random_state));
    memset(&random_buf, 0, sizeof(random_buf));

    if (initstate_r(samplerate + buffersize + oscilsize, random_state,
                    sizeof(random_state), &random_buf))
        Runtime.Log("SynthEngine Init failed on general randomness");

    if (oscilsize < (buffersize / 2))
    {
        Runtime.Log("Enforcing oscilsize to half buffersize, "
                    + asString(oscilsize) + " -> " + asString(buffersize / 2));
        oscilsize = buffersize / 2;
    }

    if (!(fft = new FFTwrapper(oscilsize)))
    {
        Runtime.Log("SynthEngine failed to allocate fft");
        goto bail_out;
    }

    if (!(vuringbuf = jack_ringbuffer_create(sizeof(VUtransfer))))
    {
        Runtime.Log("SynthEngine failed to create vu ringbuffer");
        goto bail_out;
    }

     tmpmixl = (float*)fftwf_malloc(bufferbytes);
     tmpmixr = (float*)fftwf_malloc(bufferbytes);
    if (!tmpmixl || !tmpmixr)
    {
        Runtime.Log("SynthEngine tmpmix allocations failed");
        goto bail_out;
    }

    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        part[npart] = new Part(&microtonal, fft, this);
        if (!part[npart])
        {
            Runtime.Log("Failed to allocate new Part");
            goto bail_out;
        }
        VUpeak.values.parts[npart] = -0.2;
    }

    // Insertion Effects init
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
    {
        if (!(insefx[nefx] = new EffectMgr(1, this)))
        {
            Runtime.Log("Failed to allocate new Insertion EffectMgr");
            goto bail_out;
        }
    }

    // System Effects init
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
    {
        if (!(sysefx[nefx] = new EffectMgr(0, this)))
        {
            Runtime.Log("Failed to allocate new System Effects EffectMgr");
            goto bail_out;
        }
    }
    defaults();
    if (Runtime.restoreJackSession)
    {
        if (!Runtime.restoreJsession())
        {
            Runtime.Log("Restore jack session failed");
            goto bail_out;
        }
    }
    else if (Runtime.restoreState)
    {
        if (!Runtime.stateRestore())
         {
             Runtime.Log("Restore state failed");
             goto bail_out;
         }
    }
    else
    {
        if (Runtime.paramsLoad.size()) // these are not fatal if failed
        {
            if (loadXML(Runtime.paramsLoad))
            {
                applyparameters();
                Runtime.paramsLoad = Runtime.addParamHistory(Runtime.paramsLoad);
                Runtime.Log("Loaded " + Runtime.paramsLoad + " parameters");
            }
            else
            {
                Runtime.Log("Failed to load parameters " + Runtime.paramsLoad);
                Runtime.paramsLoad = "";
            }
        }
        else if (Runtime.instrumentLoad.size())
        {
            int loadtopart = 0;
            if (part[loadtopart]->loadXMLinstrument(Runtime.instrumentLoad))
                Runtime.Log("Instrument file " + Runtime.instrumentLoad + " loaded");
            else
            {
                Runtime.Log("Failed to load instrument file " + Runtime.instrumentLoad);
                Runtime.instrumentLoad = "";
            }
        }
    }
    return true;

bail_out:
    if (fft)
        delete fft;
    fft = NULL;
    
    if (vuringbuf)
        jack_ringbuffer_free(vuringbuf);
    vuringbuf = NULL;

    if (tmpmixl)
        fftwf_free(tmpmixl);
    tmpmixl = NULL;
    if (tmpmixr)
        fftwf_free(tmpmixr);
    tmpmixr = NULL;
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        if (part[npart])
            delete part[npart];
        part[npart] = NULL;
    }
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
    {
        if (insefx[nefx])
            delete insefx[nefx];
        insefx[nefx] = NULL;
    }
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
    {
        if (sysefx[nefx])
            delete sysefx[nefx];
        sysefx[nefx] = NULL;
    }
    return false;
}


void SynthEngine::defaults(void)
{
    setPvolume(90);
    setPkeyshift(64);
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        part[npart]->defaults();
        part[npart]->Prcvchn = npart % NUM_MIDI_CHANNELS;
    }
    partonoff(0, 1); // enable the first part
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
    {
        insefx[nefx]->defaults();
        Pinsparts[nefx] = -1;
    }
    // System Effects init
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
    {
        sysefx[nefx]->defaults();
        for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
            setPsysefxvol(npart, nefx, 0);
        for (int nefxto = 0; nefxto < NUM_SYS_EFX; ++nefxto)
            setPsysefxsend(nefx, nefxto, 0);
    }
    microtonal.defaults();
    ShutUp();
}


// Note On Messages (velocity == 0 => NoteOff)
void SynthEngine::NoteOn(unsigned char chan, unsigned char note, unsigned char velocity)
{
    if (!velocity)
        this->NoteOff(chan, note);
    else if (!isMuted())
        for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        {
            if (chan == part[npart]->Prcvchn)
            {
               if (part[npart]->Penabled)
                {
                    actionLock(lock);
                    part[npart]->NoteOn(note, velocity, keyshift);
                    actionLock(unlock);
                }
                else if (VUpeak.values.parts[npart] > (-velocity))
                    VUpeak.values.parts[npart] = -(0.2 + velocity); // ensure fake is always negative
            }
        }
}


// Note Off Messages
void SynthEngine::NoteOff(unsigned char chan, unsigned char note)
{
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        if (chan == part[npart]->Prcvchn && part[npart]->Penabled)
        {
            actionLock(lock);
            part[npart]->NoteOff(note);
            actionLock(unlock);
        }
    }
}


// Controllers
void SynthEngine::SetController(unsigned char chan, int type, short int par)
{
    if (type == Runtime.midi_bank_C) {
        SetBank(par); //shouldn't get here. Banks are set directly via SetBank method from MusicIO class
    }
    else
    { // bank change doesn't directly affect parts.
        for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        {   // Send the controller to all part assigned to the channel
            if (chan == part[npart]->Prcvchn && part[npart]->Penabled)
                part[npart]->SetController(type, par);
        }

        if (type == C_allsoundsoff)
        {   // cleanup insertion/system FX
            for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
                sysefx[nefx]->cleanup();
            for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
                insefx[nefx]->cleanup();
        }
    }
}


void SynthEngine::SetBankRoot(int rootnum)
{
    if(bank.setCurrentRootID(rootnum))
    {
        Runtime.Log("Found (" + asString(rootnum) + ")  " + bank.getRootPath(bank.getCurrentRootID()));
    }
    else
    {
        Runtime.Log("No match for root ID " + asString(rootnum));
    }
    if (Runtime.showGui)
    {
        guiMaster->updateBankRootDirs();
    }


}


void SynthEngine::SetBank(int banknum)
{
    /*  we use either msb or lsb for bank changes
    128 banks is enough for anybody :-)
    this is configurable to suit different hardware synths
    */

    //new implementation uses only 1 call :)
    if(bank.setCurrentBankID(banknum))
    {
        if(Runtime.showGui && guiMaster && guiMaster->bankui)
        {
            guiMaster->bankui->set_bank_slot();
            guiMaster->bankui->refreshmainwindow();
        }
        Runtime.Log("SynthEngine setBank: Loaded bank " + asString(banknum));

    }
    else
    {
        Runtime.Log("SynthEngine setBank: Can't load bank " + asString(banknum));
    }


    /*
    int offset = 0;
#warning this needs improving.
    for (int idx = 0; idx < MAX_NUM_BANKS; ++ idx)
    {
        if (bank.banks[idx].ID == Runtime.currentRootID and offset == banknum)
        {
            banknum = idx; // Set absolute bank location
            break;
        }
        else if (bank.banks[idx].ID == Runtime.currentRootID)
            offset += 1;
    }

    if(banknum <= MAX_NUM_BANKS) {
        if (bank.loadbank(bank.banks[banknum].dir))
            Runtime.Log("SynthEngine setBank: No bank " + asString(banknum));
        if (Runtime.showGui)
        {
            guiMaster->bankui->set_bank_slot();
            guiMaster->bankui->refreshmainwindow();
        }
        else
            Runtime.Log("SynthEngine setBank: No bank " + asString(banknum));
    }
    else
        Runtime.Log("SynthEngine setBank: Value is out of range!");
    return;
    */
}


void SynthEngine::SetProgram(unsigned char chan, unsigned char pgm)
{
    bool partOK = false;
    if (bank.getname(pgm) < "!") // can't get a program name less than this
    {
        Runtime.Log("SynthEngine setProgram: No Program " + asString(pgm));
    }
    else
    {
        for(int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
            if(chan == part[npart]->Prcvchn)
            {
                partOK = bank.loadfromslot(pgm, part[npart]); // Programs indexes start from 0
                if (partOK and part[npart]->Penabled == 0 and Runtime.enable_part_on_voice_load != 0)
                {
                    partonoff(npart, 1);
                    if (Runtime.showGui)
                    {
                        guiMaster->partui->partgroupui->activate();
                        guiMaster->partui->partGroupEnable->value(1);
                    }
                }
           }
        if (partOK){
            Runtime.Log("SynthEngine setProgram: Loaded " + bank.getname(pgm));
            // update UI
            if (Runtime.showGui)
            {
                guiMaster->updatepanel();
                if (guiMaster->partui && guiMaster->partui->instrumentlabel && guiMaster->partui->part)
                    guiMaster->partui->instrumentlabel->copy_label(guiMaster->partui->part->Pname.c_str());
            }
        }
        else
            Runtime.Log("SynthEngine setProgram: Invalid program");
    }
}


// Enable/Disable a part
void SynthEngine::partonoff(int npart, int what)
{
    if (npart >= NUM_MIDI_PARTS)
        return;
    if (what)
    {
        VUpeak.values.parts[npart] = 1e-9f;
        part[npart]->Penabled = 1;
    }
    else
    {   // disabled part
        part[npart]->Penabled = 0;
        part[npart]->cleanup();
        for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
            if (Pinsparts[nefx] == npart)
                insefx[nefx]->cleanup();
        VUpeak.values.parts[npart] = -0.2;
    }
}


// Master audio out (the final sound)
void SynthEngine::MasterAudio(float *outl [NUM_MIDI_PARTS], float *outr [NUM_MIDI_PARTS], int to_process)
{    

    p_buffersize = buffersize;
    p_bufferbytes = bufferbytes;
    p_buffersize_f = buffersize_f;

    if(to_process > 0)
    {
        p_buffersize = to_process;
        p_bufferbytes = p_buffersize * sizeof(float);
        p_buffersize_f = p_buffersize;

        if(p_buffersize + processOffset > buffersize)
        {
            p_buffersize = buffersize;
            p_bufferbytes = bufferbytes;
            p_buffersize_f = buffersize_f;
        }
    }

    int npart;
    for (npart = 0; npart < (NUM_MIDI_PARTS + 1); ++npart) // include mains
    {
        memset(outl[npart], 0, p_bufferbytes);
        memset(outr[npart], 0, p_bufferbytes);
    }

    if (!isMuted())
    {

        actionLock(lock);

        // Compute part samples and store them ->partoutl,partoutr
        for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
            if (part[npart]->Penabled)
                part[npart]->ComputePartSmps();

        // Insertion effects
        int nefx;
        for (nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        {
            if (Pinsparts[nefx] >= 0)
            {
                int efxpart = Pinsparts[nefx];
                if (part[efxpart]->Penabled)
                    insefx[nefx]->out(part[efxpart]->partoutl, part[efxpart]->partoutr);
            }
        }

        // Apply the part volumes and pannings (after insertion effects)
        for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        {
            if (!part[npart]->Penabled)
                continue;

            float oldvol_l = part[npart]->oldvolumel;
            float oldvol_r = part[npart]->oldvolumer;
            float newvol_l = part[npart]->pannedVolLeft();
            float newvol_r = part[npart]->pannedVolRight();
            if (aboveAmplitudeThreshold(oldvol_l, newvol_l) || aboveAmplitudeThreshold(oldvol_r, newvol_r))
            {   // the volume or the panning has changed and needs interpolation
                for (int i = 0; i < p_buffersize; ++i)
                {
                    float vol_l = interpolateAmplitude(oldvol_l, newvol_l, i, p_buffersize);
                    float vol_r = interpolateAmplitude(oldvol_r, newvol_r, i, p_buffersize);
                    part[npart]->partoutl[i] *= vol_l;
                    part[npart]->partoutr[i] *= vol_r;
                }
                part[npart]->oldvolumel = newvol_l;
                part[npart]->oldvolumer = newvol_r;
            }
            else
            {
                for (int i = 0; i < p_buffersize; ++i)
                {   // the volume did not change
                    part[npart]->partoutl[i] *= newvol_l;
                    part[npart]->partoutr[i] *= newvol_r;
                }
            }
        }
        // System effects
        for (nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
        {
            if (!sysefx[nefx]->geteffect())
                continue; // is disabled

            // Clean up the samples used by the system effects
            memset(tmpmixl, 0, p_bufferbytes);
            memset(tmpmixr, 0, p_bufferbytes);

            // Mix the channels according to the part settings about System Effect
            for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
            {
                // skip if part is disabled, doesn't go to main or has no output to effect
                if (part[npart]->Penabled && Psysefxvol[nefx][npart]&& part[npart]->Paudiodest & 1)
                {
                    // the output volume of each part to system effect
                    float vol = sysefxvol[nefx][npart];
                    for (int i = 0; i < p_buffersize; ++i)
                    {
                        tmpmixl[i] += part[npart]->partoutl[i] * vol;
                        tmpmixr[i] += part[npart]->partoutr[i] * vol;
                    }
                }
            }

            // system effect send to next ones
            for (int nefxfrom = 0; nefxfrom < nefx; ++nefxfrom)
            {
                if (Psysefxsend[nefxfrom][nefx])
                {
                    float v = sysefxsend[nefxfrom][nefx];
                    for (int i = 0; i < p_buffersize; ++i)
                    {
                        tmpmixl[i] += sysefx[nefxfrom]->efxoutl[i] * v;
                        tmpmixr[i] += sysefx[nefxfrom]->efxoutr[i] * v;
                    }
                }
            }
            sysefx[nefx]->out(tmpmixl, tmpmixr);

            // Add the System Effect to sound output
            float outvol = sysefx[nefx]->sysefxgetvolume();
            for (int i = 0; i < p_buffersize; ++i)
            {
                outl[NUM_MIDI_PARTS][i] += tmpmixl[i] * outvol;
                outr[NUM_MIDI_PARTS][i] += tmpmixr[i] * outvol;
            }
        }

        for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        {
            if (part[npart]->Paudiodest & 2){    // Copy separate parts

                for (int i = 0; i < p_buffersize; ++i)
                {
                    outl[npart][i] = part[npart]->partoutl[i];
                    outr[npart][i] = part[npart]->partoutr[i];
                }
            }
            if (part[npart]->Paudiodest & 1)    // Mix wanted parts to mains
            {
                for (int i = 0; i < p_buffersize; ++i)
                {   // the volume did not change
                    outl[NUM_MIDI_PARTS][i] += part[npart]->partoutl[i];
                    outr[NUM_MIDI_PARTS][i] += part[npart]->partoutr[i];
                }
            }
        }

        // Insertion effects for Master Out
        for (nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        {
            if (Pinsparts[nefx] == -2)
                insefx[nefx]->out(outl[NUM_MIDI_PARTS], outr[NUM_MIDI_PARTS]);
        }

        LFOtime++; // update the LFO's time

        // Master volume, and all output fade
        float fade;
        for (int idx = 0; idx < p_buffersize; ++idx)
        {
            outl[NUM_MIDI_PARTS][idx] *= volume; // apply Master Volume
            outr[NUM_MIDI_PARTS][idx] *= volume;
            if (shutup) // fade-out
            {
                fade = (float) (p_buffersize - idx) / (float) p_buffersize;
                for (npart = 0; npart < (NUM_MIDI_PARTS + 1); ++npart) // include mains
                {
                    outl[npart][idx] *= fade;
                    outr[npart][idx] *= fade;
                }
            }
        }

        actionLock(unlock);

        // Peak calculation for mixed outputs
        VUpeak.values.vuRmsPeakL = 1e-12f;
        VUpeak.values.vuRmsPeakR = 1e-12f;
        float absval;
        for (int idx = 0; idx < p_buffersize; ++idx)
        {
            if ((absval = fabsf(outl[NUM_MIDI_PARTS][idx])) > VUpeak.values.vuOutPeakL)
                VUpeak.values.vuOutPeakL = absval;
            if ((absval = fabsf(outr[NUM_MIDI_PARTS][idx])) > VUpeak.values.vuOutPeakR)
                VUpeak.values.vuOutPeakR = absval;

            // RMS Peak
            VUpeak.values.vuRmsPeakL += outl[NUM_MIDI_PARTS][idx] * outl[NUM_MIDI_PARTS][idx];
            VUpeak.values.vuRmsPeakR += outr[NUM_MIDI_PARTS][idx] * outr[NUM_MIDI_PARTS][idx];
        }

        if (shutup)
            ShutUp();

        // Peak computation for part vu meters
        for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        {
            if (part[npart]->Penabled)
            {
                for (int idx = 0; idx < p_buffersize; ++idx)
                {
                    if ((absval = fabsf(part[npart]->partoutl[idx])) > VUpeak.values.parts[npart])
                        VUpeak.values.parts[npart] = absval;
                    if ((absval = fabsf(part[npart]->partoutr[idx])) > VUpeak.values.parts[npart])
                        VUpeak.values.parts[npart] = absval;
                }
            }
        }

        VUpeak.values.p_buffersize = p_buffersize;

        if (jack_ringbuffer_write_space(vuringbuf) >= sizeof(VUtransfer))
        {
            jack_ringbuffer_write(vuringbuf, ( char*)VUpeak.bytes, sizeof(VUtransfer));
            VUpeak.values.vuOutPeakL = 1e-12f;
            VUpeak.values.vuOutPeakR = 1e-12f;
            for (npart = 0; npart < NUM_MIDI_PARTS; ++npart)
            {
                if (part[npart]->Penabled)
                    VUpeak.values.parts[npart] = 1.0e-9;
                else if (VUpeak.values.parts[npart] < -2.2) // fake peak is a negative value
                    VUpeak.values.parts[npart]+= 2;
            }
        }
    }

    processOffset += p_buffersize;
    if(processOffset >= buffersize)
        processOffset = 0;
}


bool SynthEngine::fetchMeterData(VUtransfer *VUdata)
{
    if (jack_ringbuffer_read_space(vuringbuf) >= sizeof(VUtransfer))
    {

        jack_ringbuffer_read(vuringbuf, ( char*)VUdata->bytes, sizeof(VUtransfer));
        VUdata->values.vuRmsPeakL = sqrt(VUdata->values.vuRmsPeakL / VUdata->values.p_buffersize);
        VUdata->values.vuRmsPeakR = sqrt(VUdata->values.vuRmsPeakR / VUdata->values.p_buffersize);
        return true;
    }
    return false;
}

// Parameter control
void SynthEngine::setPvolume(char control_value)
{
    Pvolume = control_value;
    volume  = dB2rap((Pvolume - 96.0f) / 96.0f * 40.0f);
}


void SynthEngine::setPkeyshift(int Pkeyshift_)
{
    Pkeyshift = Pkeyshift_;
    keyshift = Pkeyshift - 64;
}


void SynthEngine::setPsysefxvol(int Ppart, int Pefx, char Pvol)
{
    Psysefxvol[Pefx][Ppart] = Pvol;
    sysefxvol[Pefx][Ppart]  = powf(0.1f, (1.0f - Pvol / 96.0f) * 2.0f);
}


void SynthEngine::setPsysefxsend(int Pefxfrom, int Pefxto, char Pvol)
{
    Psysefxsend[Pefxfrom][Pefxto] = Pvol;
    sysefxsend[Pefxfrom][Pefxto]  = powf(0.1f, (1.0f - Pvol / 96.0f) * 2.0f);
}

void SynthEngine::setPaudiodest(int value)
{
    Paudiodest = value;
}


// Panic! (Clean up all parts and effects)
void SynthEngine::ShutUp(void)
{    
    VUpeak.values.vuOutPeakL = 1e-12f;
    VUpeak.values.vuOutPeakR = 1e-12f;
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        part[npart]->cleanup();
        VUpeak.values.parts[npart] = -0.2;
    }
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        insefx[nefx]->cleanup();
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
        sysefx[nefx]->cleanup();
    shutup = false;
}


bool SynthEngine::actionLock(lockset request)
{
    int chk  = -1;
    switch (request)
    {
        case trylock:
            chk = pthread_mutex_trylock(processLock);
            break;

        case lock:
            chk = pthread_mutex_lock(processLock);
            break;

        case unlock:
            Unmute();
            chk = pthread_mutex_unlock(processLock);
            break;

        case lockmute:
            Mute();
            chk = pthread_mutex_lock(processLock);
            break;

        default:
            break;
    }
    return (chk == 0) ? true : false;
}


void SynthEngine::applyparameters(void)
{
    ShutUp();
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
        part[npart]->applyparameters();
}


int SynthEngine::loadParameters(string fname)
{
    int result = 0;
    Runtime.SimpleCheck = false;
    actionLock(lockmute);
    defaults(); // clear all parameters
    if(loadXML(fname)) // load the data
    {
        result = 1; // this is messy, but can't trust bool to int conversions
        if (Runtime.SimpleCheck)
            result = 3;
    }
    actionLock(unlock);
    return result;
}


void SynthEngine::add2XML(XMLwrapper *xml)
{
    xml->beginbranch("MASTER");
    actionLock(lockmute);
    xml->addpar("volume", Pvolume);
    xml->addpar("key_shift", Pkeyshift);

    xml->beginbranch("MICROTONAL");
    microtonal.add2XML(xml);
    xml->endbranch();

    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        xml->beginbranch("PART",npart);
        part[npart]->add2XML(xml);
        xml->endbranch();
    }

    xml->beginbranch("SYSTEM_EFFECTS");
    for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
    {
        xml->beginbranch("SYSTEM_EFFECT", nefx);
        xml->beginbranch("EFFECT");
        sysefx[nefx]->add2XML(xml);
        xml->endbranch();

        for (int pefx = 0; pefx < NUM_MIDI_PARTS; ++pefx)
        {
            xml->beginbranch("VOLUME", pefx);
            xml->addpar("vol", Psysefxvol[nefx][pefx]);
            xml->endbranch();
        }

        for (int tonefx = nefx + 1; tonefx < NUM_SYS_EFX; ++tonefx)
        {
            xml->beginbranch("SENDTO", tonefx);
            xml->addpar("send_vol", Psysefxsend[nefx][tonefx]);
            xml->endbranch();
        }
        xml->endbranch();
    }
    xml->endbranch();

    xml->beginbranch("INSERTION_EFFECTS");
    for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
    {
        xml->beginbranch("INSERTION_EFFECT", nefx);
        xml->addpar("part", Pinsparts[nefx]);

        xml->beginbranch("EFFECT");
        insefx[nefx]->add2XML(xml);
        xml->endbranch();
        xml->endbranch();
    }
    xml->endbranch(); // INSERTION_EFFECTS
    actionLock(unlock);
    xml->endbranch(); // MASTER
}


int SynthEngine::getalldata(char **data)
{
    XMLwrapper *xml = new XMLwrapper(this);
    add2XML(xml);
    *data = xml->getXMLdata();
    delete xml;
    return strlen(*data) + 1;
}


void SynthEngine::putalldata(const char *data, int size)
{
    XMLwrapper *xml = new XMLwrapper(this);
    if (!xml->putXMLdata(data))
    {
        Runtime.Log("SynthEngine putXMLdata failed");
        delete xml;
        return;
    }
    //if (xml->enterbranch("MASTER"))
    //{
        actionLock(lock);
        defaults();
        getfromXML(xml);
        actionLock(unlock);
        xml->exitbranch();
    //}
    //else
        //Runtime.Log("Master putAllData failed to enter MASTER branch");
    delete xml;
}


bool SynthEngine::saveXML(string filename)
{
    XMLwrapper *xml = new XMLwrapper(this);
    add2XML(xml);
    bool result = xml->saveXMLfile(filename);
    delete xml;
    return result;
}


bool SynthEngine::loadXML(string filename)
{
    XMLwrapper *xml = new XMLwrapper(this);
    if (NULL == xml)
    {
        Runtime.Log("failed to init xml tree");
        return false;
    }
    if (!xml->loadXMLfile(filename))
    {
        delete xml;
        return false;
    }
    defaults();
    bool isok = getfromXML(xml);
    delete xml;
    return isok;
}


bool SynthEngine::getfromXML(XMLwrapper *xml)
{
    if (!xml->enterbranch("MASTER"))
    {
        Runtime.Log("SynthEngine getfromXML, no MASTER branch");
        return false;
    }
    setPvolume(xml->getpar127("volume", Pvolume));
    setPkeyshift(xml->getpar127("key_shift", Pkeyshift));

    part[0]->Penabled = 0;
    for (int npart = 0; npart < NUM_MIDI_PARTS; ++npart)
    {
        if (!xml->enterbranch("PART", npart))
            continue;
        part[npart]->getfromXML(xml);
        xml->exitbranch();
    }

    if (xml->enterbranch("MICROTONAL"))
    {
        microtonal.getfromXML(xml);
        xml->exitbranch();
    }

    sysefx[0]->changeeffect(0);
    if (xml->enterbranch("SYSTEM_EFFECTS"))
    {
        for (int nefx = 0; nefx < NUM_SYS_EFX; ++nefx)
        {
            if (!xml->enterbranch("SYSTEM_EFFECT", nefx))
                continue;
            if (xml->enterbranch("EFFECT"))
            {
                sysefx[nefx]->getfromXML(xml);
                xml->exitbranch();
            }

            for (int partefx = 0; partefx < NUM_MIDI_PARTS; ++partefx)
            {
                if (!xml->enterbranch("VOLUME", partefx))
                    continue;
                setPsysefxvol(partefx, nefx,xml->getpar127("vol", Psysefxvol[partefx][nefx]));
                xml->exitbranch();
            }

            for (int tonefx = nefx + 1; tonefx < NUM_SYS_EFX; ++tonefx)
            {
                if (!xml->enterbranch("SENDTO", tonefx))
                    continue;
                setPsysefxsend(nefx, tonefx, xml->getpar127("send_vol", Psysefxsend[nefx][tonefx]));
                xml->exitbranch();
            }
            xml->exitbranch();
        }
        xml->exitbranch();
    }

    if (xml->enterbranch("INSERTION_EFFECTS"))
    {
        for (int nefx = 0; nefx < NUM_INS_EFX; ++nefx)
        {
            if (!xml->enterbranch("INSERTION_EFFECT", nefx))
                continue;
            Pinsparts[nefx] = xml->getpar("part", Pinsparts[nefx], -2, NUM_MIDI_PARTS);
            if (xml->enterbranch("EFFECT"))
            {
                insefx[nefx]->getfromXML(xml);
                xml->exitbranch();
            }
            xml->exitbranch();
        }
        xml->exitbranch();
    }
    xml->exitbranch(); // MASTER
    return true;
}


float SynthHelper::getDetune(unsigned char type, unsigned short int coarsedetune,
                             unsigned short int finedetune) const
{
    float det = 0.0f;
    float octdet = 0.0f;
    float cdet = 0.0f;
    float findet = 0.0f;

    int octave = coarsedetune / 1024; // get Octave
    if (octave >= 8)
        octave -= 16;
    octdet = octave * 1200.0f;

    int cdetune = coarsedetune % 1024; // coarse and fine detune
    if (cdetune > 512)
        cdetune -= 1024;
    int fdetune = finedetune - 8192;

    switch (type)
    {
        // case 1 is used for the default (see below)
        case 2:
            cdet = fabs(cdetune * 10.0f);
            findet = fabs(fdetune / 8192.0f) * 10.0f;
            break;
        case 3:
            cdet = fabsf(cdetune * 100.0f);
            findet = powf(10.0f, fabs(fdetune / 8192.0f) * 3.0f) / 10.0f - 0.1f;
            break;
        case 4:
            cdet = fabs(cdetune * 701.95500087f); // perfect fifth
            findet = (powf(2.0f, fabs(fdetune / 8192.0f) * 12.0f) - 1.0f) / 4095.0f * 1200.0f;
            break;
            // case ...: need to update N_DETUNE_TYPES, if you'll add more
        default:
            cdet = fabs(cdetune * 50.0f);
            findet = fabs(fdetune / 8192.0f) * 35.0f; // almost like "Paul's Sound Designer 2"
            break;
    }
    if (finedetune < 8192)
        findet = -findet;
    if (cdetune < 0)
        cdet = -cdet;
    det = octdet + cdet + findet;
    return det;
}

MasterUI *SynthEngine::getGuiMaster(bool createGui)
{
    if(guiMaster == NULL && createGui)
    {
        guiMaster = new MasterUI(this);
    }
    return guiMaster;
}

void SynthEngine::guiClosed(bool stopSynth)
{
    if(stopSynth && !isLV2Plugin)
        Runtime.runSynth = false;    
    if(guiClosedCallback != NULL)
        guiClosedCallback(guiCallbackArg);
}

void SynthEngine::closeGui()
{
    if(guiMaster != NULL)
    {
        delete guiMaster;
        guiMaster = NULL;
    }
}

std::string SynthEngine::makeUniqueName(const char *name)
{
    char strUniquePostfix [1024];
    std::string newUniqueName = name;
    memset(strUniquePostfix, 0, sizeof(strUniquePostfix));
    if(uniqueId > 0)
    {
        snprintf(strUniquePostfix, sizeof(strUniquePostfix), "-%d", uniqueId);
    }

    newUniqueName += strUniquePostfix;
    return newUniqueName;
}

