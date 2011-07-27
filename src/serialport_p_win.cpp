/*
    License...
*/

#include "serialport_p.h"

#include <QtCore/QRegExp>

#ifndef Q_CC_MSVC
#  include <ddk/ntddser.h>
#else
#ifndef IOCTL_SERIAL_GET_DTRRTS
#define IOCTL_SERIAL_GET_DTRRTS \
    CTL_CODE (FILE_DEVICE_SERIAL_PORT, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif //Q_CC_MSVC

#ifndef SERIAL_DTR_STATE
#  define SERIAL_DTR_STATE  0x00000001
#endif

#ifndef SERIAL_RTS_STATE
#  define SERIAL_RTS_STATE  0x00000002
#endif

#endif


/* Public methods */


SerialPortPrivate::SerialPortPrivate()
    : AbstractSerialPortPrivate()
    , m_descriptor(INVALID_HANDLE_VALUE)
{
    size_t size = sizeof(DCB);
    ::memset(&m_currDCB, 0, size);
    ::memset(&m_oldDCB, 0, size);
    size = sizeof(COMMTIMEOUTS);
    ::memset(&m_currCommTimeouts, 0, size);
    ::memset(&m_oldCommTimeouts, 0, size);
    size = sizeof(OVERLAPPED);
    ::memset(&m_ovRead, 0, size);
    ::memset(&m_ovWrite, 0, size);
    ::memset(&m_ovSelect, 0, size);

    m_notifier.setRef(this);
}

bool SerialPortPrivate::open(QIODevice::OpenMode mode)
{
    DWORD access = 0;
    DWORD sharing = 0;
    bool rxflag = false;
    bool txflag = false;

    if (mode & QIODevice::ReadOnly) {
        access |= GENERIC_READ; //sharing = FILE_SHARE_READ;
        rxflag = true;
    }
    if (mode & QIODevice::WriteOnly) {
        access |= GENERIC_WRITE; //sharing = FILE_SHARE_WRITE;
        txflag = true;
    }

    QByteArray nativeFilePath = QByteArray((const char *)m_systemLocation.utf16(),
                                           m_systemLocation.size() * 2 + 1);

    // Try opened serial device.
    m_descriptor = ::CreateFile((const wchar_t*)nativeFilePath.constData(),
                                access, sharing, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);

    if (m_descriptor == INVALID_HANDLE_VALUE) {
        switch (::GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
            setError(SerialPort::NoSuchDeviceError); break;
        case ERROR_ACCESS_DENIED:
            setError(SerialPort::PermissionDeniedError); break;
        default:
            setError(SerialPort::UnknownPortError);
        }
        return false;
    }

    if (saveOldsettings()) {
        // Call before updateDcb().
        prepareOtherOptions();

        if (updateDcb()) {
            prepareCommTimeouts(SerialPortPrivate::ReadIntervalTimeout, MAXWORD);
            prepareCommTimeouts(SerialPortPrivate::ReadTotalTimeoutMultiplier, 0);
            prepareCommTimeouts(SerialPortPrivate::ReadTotalTimeoutConstant, 0);
            prepareCommTimeouts(SerialPortPrivate::WriteTotalTimeoutMultiplier, 0);
            prepareCommTimeouts(SerialPortPrivate::WriteTotalTimeoutConstant, 0);

            if (updateCommTimeouts()) {
                // Disable autocalculate total read interval.
                // isAutoCalcReadTimeoutConstant = false;

                if (createEvents(rxflag, txflag)) {
                    detectDefaultSettings();
                    return true;
                }
            }
        }
    }
    setError(SerialPort::ConfiguringError);
    return false;
}

void SerialPortPrivate::close()
{
    ::CancelIo(m_descriptor);
    restoreOldsettings();
    ::CloseHandle(m_descriptor);
    closeEvents();
    m_descriptor = INVALID_HANDLE_VALUE;
}

SerialPort::Lines SerialPortPrivate::lines() const
{
    DWORD modemStat = 0;
    SerialPort::Lines ret = 0;

    if (::GetCommModemStatus(m_descriptor, &modemStat) == 0) {
        // Print error?
        return ret;
    }

    if (modemStat & MS_CTS_ON)
        ret |= SerialPort::Cts;
    if (modemStat & MS_DSR_ON)
        ret |= SerialPort::Dsr;
    if (modemStat & MS_RING_ON)
        ret |= SerialPort::Ri;
    if (modemStat & MS_RLSD_ON)
        ret |= SerialPort::Dcd;

    DWORD bytesReturned = 0;
    if (::DeviceIoControl(m_descriptor, IOCTL_SERIAL_GET_DTRRTS, 0, 0,
                          &modemStat, sizeof(DWORD),
                          &bytesReturned, 0)) {

        if (modemStat & SERIAL_DTR_STATE)
            ret |= SerialPort::Dtr;
        if (modemStat & SERIAL_RTS_STATE)
            ret |= SerialPort::Rts;
    }

    return ret;
}

bool SerialPortPrivate::setDtr(bool set)
{
    bool ret = ::EscapeCommFunction(m_descriptor, (set) ? SETDTR : CLRDTR);
    if (!ret) {
        // FIXME: Here need call ::GetLastError()
        // and set error type.
    }
    return ret;
}

bool SerialPortPrivate::setRts(bool set)
{
    bool ret = ::EscapeCommFunction(m_descriptor, (set) ? SETRTS : CLRRTS);
    if (!ret) {
        // FIXME: Here need call ::GetLastError()
        // and set error type.
    }
    return ret;
}

bool SerialPortPrivate::reset()
{
    DWORD flags = (PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
    bool ret = ::PurgeComm(m_descriptor, flags);
    if (!ret) {
        // FIXME: Here need call ::GetLastError()
        // and set error type.
    }
    return ret;
}

bool SerialPortPrivate::sendBreak(int duration)
{
    // FIXME:
    if (setBreak(true)) {
        ::Sleep(DWORD(duration));
        if (setBreak(false))
            return true;
    }
    return false;
}

bool SerialPortPrivate::setBreak(bool set)
{
    bool ret = (set) ?
                (::SetCommBreak(m_descriptor)) : (::ClearCommBreak(m_descriptor));
    if (!ret) {
        // FIXME: Here need call ::GetLastError()
        // and set error type.
    }
    return ret;
}

qint64 SerialPortPrivate::bytesAvailable() const
{
    DWORD err;
    COMSTAT cs;
    if ((::ClearCommError(m_descriptor, &err, &cs) == 0)
            || err) {
        return -1;
    }
    return qint64(cs.cbInQue);
}

qint64 SerialPortPrivate::bytesToWrite() const
{
    DWORD err;
    COMSTAT cs;
    if ((::ClearCommError(m_descriptor, &err, &cs) == 0)
            || err) {
        return -1;
    }
    return qint64(cs.cbOutQue);
}

// Clear overlapped structure, but does not affect the event.
static void clear_overlapped(OVERLAPPED *overlapped)
{
    overlapped->Internal = 0;
    overlapped->InternalHigh = 0;
    overlapped->Offset = 0;
    overlapped->OffsetHigh = 0;
}

qint64 SerialPortPrivate::read(char *data, qint64 len)
{
    clear_overlapped(&m_ovRead);

    DWORD readBytes = 0;
    bool sucessResult = false;

    if (::ReadFile(m_descriptor, (PVOID)data, (DWORD)len, &readBytes, &m_ovRead))
        sucessResult = true;
    else {
        if (::GetLastError() == ERROR_IO_PENDING) {
            // Instead of an infinite wait I/O (not looped), we expect, for example 5 seconds.
            // Although, maybe there is a better solution.
            switch (::WaitForSingleObject(m_ovRead.hEvent, 5000)) {
            case WAIT_OBJECT_0: {
                if (::GetOverlappedResult(m_descriptor, &m_ovRead, &readBytes, false))
                    sucessResult = true;
            }
            break;
            default: ;
            }
        }
    }
    if(!sucessResult) {
        // FIXME: Here need call ::GetLastError()
        // and set error type?
        setError(SerialPort::IoError);
        return -1;
    }
    return qint64(readBytes);
}

qint64 SerialPortPrivate::write(const char *data, qint64 len)
{
    clear_overlapped(&m_ovWrite);

    DWORD writeBytes = 0;
    bool sucessResult = false;

    if (::WriteFile(m_descriptor, (LPCVOID)data, (DWORD)len, &writeBytes, &m_ovWrite))
        sucessResult = true;
    else {
        if (::GetLastError() == ERROR_IO_PENDING) {
            // Instead of an infinite wait I/O (not looped), we expect, for example 5 seconds.
            // Although, maybe there is a better solution.
            switch (::WaitForSingleObject(m_ovWrite.hEvent, 5000)) {
            case WAIT_OBJECT_0: {
                if (::GetOverlappedResult(m_descriptor, &m_ovWrite, &writeBytes, false))
                    sucessResult = true;
            }
            break;
            default: ;
            }
        }
    }
    if(!sucessResult) {
        // FIXME: Here need call ::GetLastError()
        // and set error type?
        setError(SerialPort::IoError);
        return -1;
    }
    return quint64(writeBytes);
}

bool SerialPortPrivate::waitForReadOrWrite(int timeout,
                                           bool checkRead, bool checkWrite,
                                           bool *selectForRead, bool *selectForWrite)
{
    // Forward checking data for read.
    if (checkRead && (bytesAvailable() > 0)) {
        *selectForRead = true;
        return 1;
    }

    clear_overlapped(&m_ovSelect);

    DWORD oldEventMask = 0;
    DWORD currEventMask = 0;

    if (checkRead)
        currEventMask |= EV_RXCHAR;
    if (checkWrite)
        currEventMask |= EV_TXEMPTY;

    // Save old mask.
    if (::GetCommMask(m_descriptor, &oldEventMask) == 0) {
        //Print error?
        return false;
    }

    // Checking the old mask bits as in the current mask.
    // And if these bits are not exists, then add them and set the reting mask.
    if (currEventMask != (oldEventMask & currEventMask)) {
        currEventMask |= oldEventMask;
        if (::SetCommMask(m_descriptor, currEventMask) == 0) {
            //Print error?
            return false;
        }
    }

    currEventMask = 0;
    bool sucessResult = false;

    if (::WaitCommEvent(m_descriptor, &currEventMask, &m_ovSelect))
        sucessResult = true;
    else {
        if (::GetLastError() == ERROR_IO_PENDING) {
            DWORD bytesTransferred = 0;
            switch (::WaitForSingleObject(m_ovSelect.hEvent, (timeout < 0) ? 0 : timeout)) {
            case WAIT_OBJECT_0: {
                if (::GetOverlappedResult(m_descriptor, &m_ovSelect, &bytesTransferred, false))
                    sucessResult = true;
            }
            break;
            default: ;
            }
        }
    }

    if (sucessResult) {
        // Here call the bytesAvailable() to protect against false positives WaitForSingleObject(),
        // for example, when manually pulling USB/Serial converter from system,
        // ie when devices are in fact not.
        // While it may be possible to make additional checks - to catch an event EV_ERR,
        // adding (in the code above) extra bits in the mask currEventMask.
        *selectForRead = checkRead && (currEventMask & EV_RXCHAR) && (bytesAvailable() > 0);
        *selectForWrite = checkWrite && (currEventMask & EV_TXEMPTY);
    }
    // Rerair old mask.
    ::SetCommMask(m_descriptor, oldEventMask);
    return sucessResult;
}


/* Protected methods */


static const QString defaultPathPrefix = "\\\\.\\";

QString SerialPortPrivate::nativeToSystemLocation(const QString &port) const
{
    QString ret;
    if (!port.contains(defaultPathPrefix))
        ret.append(defaultPathPrefix);
    ret.append(port);
    return ret;
}

QString SerialPortPrivate::nativeFromSystemLocation(const QString &location) const
{
    QString ret = location;
    if (ret.contains(defaultPathPrefix))
        ret.remove(defaultPathPrefix);
    return ret;
}

bool SerialPortPrivate::setNativeRate(qint32 rate, SerialPort::Directions dir)
{
    if ((rate == SerialPort::UnknownRate) || (dir != SerialPort::AllDirections)) {
        setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }
    m_currDCB.BaudRate = DWORD(rate);
    bool ret = updateDcb();
    if (!ret)
        setError(SerialPort::ConfiguringError);
    return ret;
}

bool SerialPortPrivate::setNativeDataBits(SerialPort::DataBits dataBits)
{
    if ((dataBits == SerialPort::UnknownDataBits)
            || isRestrictedAreaSettings(dataBits, m_stopBits)) {

        setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }
    m_currDCB.ByteSize = BYTE(dataBits);
    bool ret = updateDcb();
    if (!ret)
        setError(SerialPort::ConfiguringError);
    return ret;
}

bool SerialPortPrivate::setNativeParity(SerialPort::Parity parity)
{
    if (parity == SerialPort::UnknownParity) {
        setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }

    m_currDCB.fParity = true;
    switch (parity) {
    case SerialPort::NoParity:  {
        m_currDCB.Parity = NOPARITY;
        m_currDCB.fParity = false;
    }
    break;
    case SerialPort::SpaceParity: m_currDCB.Parity = SPACEPARITY; break;
    case SerialPort::MarkParity: m_currDCB.Parity = MARKPARITY; break;
    case SerialPort::EvenParity: m_currDCB.Parity = EVENPARITY; break;
    case SerialPort::OddParity: m_currDCB.Parity = ODDPARITY; break;
    default: return false;
    }
    bool ret = updateDcb();
    if (!ret)
        setError(SerialPort::ConfiguringError);
    return ret;
}

bool SerialPortPrivate::setNativeStopBits(SerialPort::StopBits stopBits)
{
    if ((stopBits == SerialPort::UnknownStopBits)
            || isRestrictedAreaSettings(m_dataBits, stopBits)) {

        setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }

    switch (stopBits) {
    case SerialPort::OneStop: m_currDCB.StopBits = ONESTOPBIT; break;
    case SerialPort::OneAndHalfStop: m_currDCB.StopBits = ONE5STOPBITS; break;
    case SerialPort::TwoStop: m_currDCB.StopBits = TWOSTOPBITS; break;
    default: return false;
    }
    bool ret = updateDcb();
    if (!ret)
        setError(SerialPort::ConfiguringError);
    return ret;
}

bool SerialPortPrivate::setNativeFlowControl(SerialPort::FlowControl flow)
{
    if (flow == SerialPort::UnknownFlowControl) {
        setError(SerialPort::UnsupportedPortOperationError);
        return false;
    }

    switch (flow) {
    case SerialPort::NoFlowControl: {
        m_currDCB.fOutxCtsFlow = false;
        m_currDCB.fRtsControl = RTS_CONTROL_DISABLE;
        m_currDCB.fInX = m_currDCB.fOutX = false;
    }
    break;
    case SerialPort::SoftwareControl: {
        m_currDCB.fOutxCtsFlow = false;
        m_currDCB.fRtsControl = RTS_CONTROL_DISABLE;
        m_currDCB.fInX = m_currDCB.fOutX = true;
    }
    break;
    case SerialPort::HardwareControl: {
        m_currDCB.fOutxCtsFlow = true;
        m_currDCB.fRtsControl = RTS_CONTROL_HANDSHAKE;
        m_currDCB.fInX = m_currDCB.fOutX = false;
    }
    break;
    default: return false;
    }
    bool ret = updateDcb();
    if (!ret)
        setError(SerialPort::ConfiguringError);
    return ret;
}

bool SerialPortPrivate::setNativeDataInterval(int usecs)
{
    // Impl me
    return false;
}

bool SerialPortPrivate::setNativeReadTimeout(int msecs)
{
    // Impl me
    return false;
}

bool SerialPortPrivate::setNativeDataErrorPolicy(SerialPort::DataErrorPolicy policy)
{
    // Impl me
    return false;
}

bool SerialPortPrivate::nativeFlush()
{
    bool ret = ::FlushFileBuffers(m_descriptor);
    if (!ret) {
        // FIXME: Here need call ::GetLastError()
        // and set error type.
    }
    return ret;
}

void SerialPortPrivate::detectDefaultSettings()
{
    // Detect rate.
    m_inRate = quint32(m_currDCB.BaudRate);
    m_outRate = m_inRate;

    // Detect databits.
    switch (m_currDCB.ByteSize) {
    case 5: m_dataBits = SerialPort::Data5; break;
    case 6: m_dataBits = SerialPort::Data6; break;
    case 7: m_dataBits = SerialPort::Data7; break;
    case 8: m_dataBits = SerialPort::Data8; break;
    default: m_dataBits = SerialPort::UnknownDataBits;
    }

    // Detect parity.
    if ((m_currDCB.Parity == NOPARITY) && (!m_currDCB.fParity))
        m_parity = SerialPort::NoParity;
    else if ((m_currDCB.Parity == SPACEPARITY) && m_currDCB.fParity)
        m_parity = SerialPort::SpaceParity;
    else if ((m_currDCB.Parity == MARKPARITY) && m_currDCB.fParity)
        m_parity = SerialPort::MarkParity;
    else if ((m_currDCB.Parity == EVENPARITY) && m_currDCB.fParity)
        m_parity = SerialPort::EvenParity;
    else if ((m_currDCB.Parity == ODDPARITY) && m_currDCB.fParity)
        m_parity = SerialPort::OddParity;
    else
        m_parity = SerialPort::UnknownParity;

    // Detect stopbits.
    switch (m_currDCB.StopBits) {
    case ONESTOPBIT: m_stopBits = SerialPort::OneStop; break;
    case ONE5STOPBITS: m_stopBits = SerialPort::OneAndHalfStop; break;
    case TWOSTOPBITS: m_stopBits = SerialPort::TwoStop; break;
    default: m_stopBits = SerialPort::UnknownStopBits;
    }

    // Detect flow control.
    if ((!m_currDCB.fOutxCtsFlow) && (m_currDCB.fRtsControl == RTS_CONTROL_DISABLE)
            && (!m_currDCB.fInX) && (!m_currDCB.fOutX)) {
        m_flow = SerialPort::NoFlowControl;
    }
    else if ((!m_currDCB.fOutxCtsFlow) && (m_currDCB.fRtsControl == RTS_CONTROL_DISABLE)
             && (m_currDCB.fInX) && (m_currDCB.fOutX)) {
        m_flow = SerialPort::SoftwareControl;
    }
    else if ((m_currDCB.fOutxCtsFlow) && (m_currDCB.fRtsControl == RTS_CONTROL_HANDSHAKE)
             && (!m_currDCB.fInX) && (!m_currDCB.fOutX)) {
        m_flow = SerialPort::HardwareControl;
    }
    else
        m_flow = SerialPort::UnknownFlowControl;
}

// Used only in method SerialPortPrivate::open().
bool SerialPortPrivate::saveOldsettings()
{
    DWORD confSize = sizeof(DCB);
    if (::GetCommState(m_descriptor, &m_oldDCB) == 0)
        return false;
    ::memcpy((void *)(&m_currDCB), (const void *)(&m_oldDCB), confSize);

    confSize = sizeof(COMMTIMEOUTS);
    if (::GetCommTimeouts(m_descriptor, &m_oldCommTimeouts) == 0)
        return false;
    ::memcpy((void *)(&m_currCommTimeouts), (const void *)(&m_oldCommTimeouts), confSize);

    m_oldSettingsIsSaved = true;
    return true;
}

// Used only in method SerialPortPrivate::close().
bool SerialPortPrivate::restoreOldsettings()
{
    bool restoreResult = true;
    if (m_oldSettingsIsSaved) {
        m_oldSettingsIsSaved = false;
        if (::GetCommState(m_descriptor, &m_oldDCB) != 0)
            restoreResult = false;
        if (::SetCommTimeouts(m_descriptor, &m_oldCommTimeouts) == 0)
            restoreResult = false;
    }
    return restoreResult;
}


/* Private methods */


bool SerialPortPrivate::createEvents(bool rx, bool tx)
{
    if (rx) { m_ovRead.hEvent = ::CreateEvent(0, false, false, 0); }
    if (tx) { m_ovWrite.hEvent = ::CreateEvent(0, false, false, 0); }
    m_ovSelect.hEvent = ::CreateEvent(0, false, false, 0);

    return ((rx && (m_ovRead.hEvent == 0))
            || (tx && (m_ovWrite.hEvent == 0))
            || (m_ovSelect.hEvent) == 0) ?
                false : true;
}

void SerialPortPrivate::closeEvents() const
{
    if (m_ovRead.hEvent)
        ::CloseHandle(m_ovRead.hEvent);
    if (m_ovWrite.hEvent)
        ::CloseHandle(m_ovWrite.hEvent);
    if (m_ovSelect.hEvent)
        ::CloseHandle(m_ovSelect.hEvent);
}

void SerialPortPrivate::recalcTotalReadTimeoutConstant()
{

}

void SerialPortPrivate::prepareCommTimeouts(CommTimeouts cto, DWORD msecs)
{
    DWORD *ptr = 0;
    switch (cto) {
    case ReadIntervalTimeout:
        ptr = &m_currCommTimeouts.ReadIntervalTimeout; break;
    case ReadTotalTimeoutMultiplier:
        ptr = &m_currCommTimeouts.ReadTotalTimeoutMultiplier; break;
    case ReadTotalTimeoutConstant:
        ptr = &m_currCommTimeouts.ReadTotalTimeoutConstant; break;
    case WriteTotalTimeoutMultiplier:
        ptr = &m_currCommTimeouts.WriteTotalTimeoutMultiplier; break;
    case WriteTotalTimeoutConstant:
        ptr = &m_currCommTimeouts.WriteTotalTimeoutConstant; break;
    default:;
    }

    if (*ptr != msecs)
        *ptr = msecs;
}

inline bool SerialPortPrivate::updateDcb()
{
    return (::SetCommState(m_descriptor, &m_currDCB) != 0);
}

inline bool SerialPortPrivate::updateCommTimeouts()
{
    return (::SetCommTimeouts(m_descriptor, &m_currCommTimeouts) != 0);
}

bool SerialPortPrivate::isRestrictedAreaSettings(SerialPort::DataBits dataBits,
                                                 SerialPort::StopBits stopBits) const
{
    return (((dataBits == SerialPort::Data5) && (stopBits == SerialPort::TwoStop))
            || ((dataBits == SerialPort::Data6) && (stopBits == SerialPort::OneAndHalfStop))
            || ((dataBits == SerialPort::Data7) && (stopBits == SerialPort::OneAndHalfStop))
            || ((dataBits == SerialPort::Data8) && (stopBits == SerialPort::OneAndHalfStop)));
}

// Prepares other parameters of the structures port configuration.
// Used only in method SerialPortPrivate::open().
void SerialPortPrivate::prepareOtherOptions()
{
    m_currDCB.fBinary = true;
    m_currDCB.fInX = false;
    m_currDCB.fOutX = false;
    m_currDCB.fAbortOnError = false;
    m_currDCB.fNull = false;
}

