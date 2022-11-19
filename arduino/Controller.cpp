#include <avr/pgmspace.h>

#include "Controller.h"
#include "Pins.h"
#include "Printer.h"

#include "uc/mModeDecoder.h"
#include "uc/mOpDecoder.h"
#include "uc/uROM_0.h"
#include "uc/uROM_1.h"
#include "uc/uROM_2.h"
#include "uc/uROM_3.h"
#include "uc/uROM_4.h"

// High and low time for a clock cycle
#define CLOCK_PULSE_DELAY_MICROS 100
#define PC_TO_MAR (PC_OUT_CADDR | MAR_LD_CADDR)

Controller::Controller(ControlLines *controlLines, EightBitBus *cdataBus)
{
    _controlLines = controlLines;
    _cdataBus = cdataBus;

    pinMode(CLOCK_PIN, OUTPUT);
    pinMode(PZ_PIN, INPUT);
    digitalWrite(CLOCK_PIN, LOW);
}

void Controller::setMCBreakpoint(byte breakpoint)
{
    _mcBreakpoint = breakpoint;
    _mcBreakpointSet = true;
}

void Controller::clearMCBreakpoint()
{
    _mcBreakpointSet = false;
}

byte Controller::getCUAddr()
{
    return _cuaddr;
}

byte Controller::getIR()
{
    return _ir;
}

byte Controller::getPC()
{
    return _pc;
}

void Controller::pulseClock()
{
    digitalWrite(CLOCK_PIN, HIGH);
    delayMicroseconds(CLOCK_PULSE_DELAY_MICROS);
    digitalWrite(CLOCK_PIN, LOW);
    delayMicroseconds(CLOCK_PULSE_DELAY_MICROS);
}

void Controller::step(unsigned long controlLineBits, unsigned int delayMicros)
{
    _controlLines->set(controlLineBits);
    pulseClock();
    delayMicroseconds(delayMicros);
}

void Controller::setMAR(byte value)
{
    Printer::Print("Setting MAR", "MAR", value, Printer::Verbosity::verbose);
    _cdataBus->set(value);
    step(MAR_LD_CADDR | CDATA_TO_CADDR);
    _cdataBus->detach();
}

byte Controller::cuaddrNext(unsigned long currentControlLines)
{
    byte result;

    if (currentControlLines & uJMP)
    {
        // Absolute jump
        result = uROM_0[_cuaddr];
    }
    else if (currentControlLines & uZJMP)
    {
        // Jump based on Z flag
        bool pz = getPZ();
        if ((currentControlLines & uJMPINV) && !pz)
        {
            // Jump on not zero
            result = uROM_0[_cuaddr];
        }
        else if (pz)
        {
            // Jump if zero
            Serial.println("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX BEQ");
            result = pgm_read_byte_near(uROM_0 + _cuaddr);
            Serial.println(_cuaddr, HEX);
            Serial.println(result, HEX);
        }
        else
        {
            // Default
            result = _cuaddr + 1;
        }
    }
    else
    {
        // Default
        result = _cuaddr + 1;
    }

    return result;
}

bool Controller::executePhaseStep(unsigned long doneFlag, bool &error)
{
    bool done = false;

    Printer::Print("cuaddr", _cuaddr, Printer::Verbosity::verbose);

    error = _cuaddr == 0xFF;

    if (!error)
    {
        _currentControlLines = makeControlLines(
            pgm_read_byte_near(uROM_4 + _cuaddr),
            pgm_read_byte_near(uROM_3 + _cuaddr),
            pgm_read_byte_near(uROM_2 + _cuaddr),
            pgm_read_byte_near(uROM_1 + _cuaddr));

        Printer::Print("Control lines", (unsigned long)_currentControlLines, Printer::Verbosity::verbose, Printer::Base::BASE_BIN, true);

        error = _currentControlLines == 0xFF;

        if (!error)
        {
            if ((_currentControlLines & PC_TO_MAR) == PC_TO_MAR)
            {
                setMAR(_pc);
                _currentControlLines &= ~(MAR_LD_CADDR | CDATA_TO_CADDR);
            }

            done = _currentControlLines & doneFlag;

            if (_currentControlLines & PC_INC)
            {
                _pc++;
                Printer::Print("Incremented PC", "PC", _pc, Printer::Verbosity::verbose);
            }
            else if (_currentControlLines & PC_REL_CDATA)
            {
                Serial.print("---------------------XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX PC REL ");
                Serial.println(_pc, HEX);
                _controlLines->set(MBR_OUT_CDATA);

                byte cdataVal = _cdataBus->read();
                Serial.println(cdataVal, HEX);
                _pc += cdataVal;
                Serial.println(_pc, HEX);
                _currentControlLines &= ~PC_REL_CDATA;
            }
            else if ((_currentControlLines & (MBR_OUT_CDATA | PC_LD_CDATA)) == (MBR_OUT_CDATA | PC_LD_CDATA))
            {
                Serial.print("---------------------XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX JMP ");
                _controlLines->set(MBR_OUT_CDATA);
                Serial.println(_pc, HEX);

                _pc = _cdataBus->read();
                Serial.println(_pc, HEX);

                _currentControlLines &= ~(MBR_OUT_CDATA | PC_LD_CDATA);
            }

            if (_currentControlLines)
            {
                step(_currentControlLines);
            }

            _cuaddr = cuaddrNext(_currentControlLines);

            if (!done)
            {
                Printer::Println(Printer::Verbosity::verbose);
            }
        }
    }

    return done;
}

bool Controller::getPZ()
{
    bool result = digitalRead(PZ_PIN);
    Serial.print("========> Z=");
    Serial.println(result);
    return result;
}

byte Controller::getA()
{
    _controlLines->set(A_OUT_CDATA);
    byte result = _cdataBus->read();
    _controlLines->set(_currentControlLines);
    return result;
}

byte Controller::getX()
{
    _controlLines->set(X_OUT_CDATA);
    byte result = _cdataBus->read();
    _controlLines->set(_currentControlLines);
    return result;
}

byte Controller::getALUOut()
{
    _controlLines->set(ALUR_OUT_CDATA);
    byte result = _cdataBus->read();
    _controlLines->set(_currentControlLines);
    return result;
}

const char *Controller::PhaseToText(Phase phase)
{
    switch (phase)
    {
    case Phase::pI:
        return "Init";
        break;
    case Phase::p0:
        return "P0 (Fetch)";
        break;
    case Phase::p1:
        return "P1 (Addr)";
        break;
    case Phase::p2:
        return "P2 (Op)";
        break;
    }
}

void Controller::announcePhaseStart(Phase phase)
{
    Printer::Print(PhaseToText(phase), Printer::Verbosity::verbose);
    Printer::Println(" --->", Printer::Verbosity::verbose);
}

void Controller::announcePhaseEnd(Phase phase)
{
    Printer::Print("<--- ", Printer::Verbosity::verbose);
    Printer::Println(PhaseToText(phase), Printer::Verbosity::verbose);
    Printer::Println(Printer::Verbosity::verbose);
}

void Controller::uStep(bool &programComplete, bool &mcBreak, bool &error)
{
    bool phaseDone = false;

    if (_newPhase)
    {
        if (_phase == Phase::p0)
        {
            Printer::Println("================================", Printer::Verbosity::verbose);
            Printer::Println(Printer::Verbosity::verbose);
        }
        announcePhaseStart(_phase);
    }

    switch (_phase)
    {
    case Phase::pI:
        if (_newPhase)
        {
            _newPhase = false;
        }

        phaseDone = executePhaseStep(uP0, error);

        if (phaseDone)
        {
            announcePhaseEnd(_phase);

            _phase = p0;
            _newPhase = true;
        }
        break;

    case Phase::p0:
        mcBreak = _mcBreakpointSet && _pc == _mcBreakpoint;

        error = p0Fetch();
        programComplete = _ir == 0;

        announcePhaseEnd(_phase);

        _phase = p1;
        _newPhase = true;

        break;
    case Phase::p1:
        if (_newPhase)
        {
            _newPhase = false;
            _cuaddr = addrModeDecode();
        }

        phaseDone = executePhaseStep(uP2, error);

        if (phaseDone)
        {
            announcePhaseEnd(_phase);
            _phase = p2;
            _newPhase = true;
        }
        break;
    case Phase::p2:
        if (_newPhase)
        {
            _newPhase = false;
            _cuaddr = opDecode();
        }
        phaseDone = executePhaseStep(uP0, error);
        if (phaseDone)
        {
            announcePhaseEnd(_phase);
            _phase = p0;
            _newPhase = true;
        }
        break;
    }
}

bool Controller::p0Fetch()
{
    bool error = false;

    Printer::Print("PC", _pc, Printer::Verbosity::verbose);
    Printer::Println(Printer::Verbosity::verbose);

    // Set MAR to pc
    setMAR(_pc);

    // Load the current instruction into the MBR
    step(MEM_OUT_XDATA | MBR_LD_XDATA);

    // Transfer the MBR into IR
    step(MBR_OUT_CDATA);
    _ir = _cdataBus->read();
    error = _ir == 0xFF;

    if (!error)
    {
        _pc++;

        Printer::Println(Printer::Verbosity::verbose);
        Printer::Print("IR", _ir, Printer::Verbosity::verbose);
    }

    return error;
}

byte Controller::addrModeDecode()
{
    byte result = pgm_read_byte_near(mModeDecoder + _ir);
    return result;
}

unsigned long Controller::makeControlLines(byte rom4, byte rom3, byte rom2, byte rom1)
{
    unsigned long result = ((unsigned long)rom4) << 24 | ((unsigned long)rom3) << 16 | ((unsigned long)rom2) << 8 | rom1;
    return result;
}

byte Controller::opDecode()
{
    byte result = pgm_read_byte_near(mOpDecoder + _ir);
    return result;
}

void Controller::reset()
{
    _phase = Phase::pI;
    _cuaddr = 0;
    _pc = 0;
}

void Controller::run()
{
    reset();
    go();
}

void Controller::go()
{
    bool done = false;
    bool error = false;
    bool mcBreak = false;

    while (!done && !error && !mcBreak)
    {
        uStep(done, mcBreak, error);
    }

    if (mcBreak)
    {
        Printer::Println();
        Printer::Print("MC Breakpoint hit at");
    }

    if (error)
    {
        Printer::Println();
        Printer::Println("===============");
        Printer::Println("==== ERROR ====");
        Printer::Println("Error condition: ");
        Printer::Print("IR", _ir, Printer::Verbosity::all, Printer::Base::BASE_HEX, false);
        Printer::Print(", PC", _pc, Printer::Verbosity::all, Printer::Base::BASE_HEX, false);
        Printer::Print(", cuaddr", _cuaddr, Printer::Verbosity::all, Printer::Base::BASE_HEX, true);
        Printer::Println("===============");
        Printer::Println();

        while (true)
        {
            delay(1000);
        }
    }
}
