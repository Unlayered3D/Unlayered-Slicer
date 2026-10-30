#ifndef slic3r_GUI_Utils_Serial_hpp_
#define slic3r_GUI_Utils_Serial_hpp_

#include <utility>
#include <boost/asio/serial_port.hpp>
#include <boost/asio/io_context.hpp>
#include <vector>
#include <string>
#include <boost/system/error_code.hpp>
#include <boost/asio.hpp>


namespace Slic3r {
namespace Utils {

struct SerialPortInfo {
	std::string port;
	unsigned    id_vendor = -1;
	unsigned    id_product = -1;
	std::string friendly_name;
	bool        is_printer = false;

	// Which physical USB socket this port is plugged into, e.g.
	// "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(6)". Empty when it cannot be determined.
	//
	// This is the only durable identity a Duet has. It reports an EMPTY USB serial number, and a
	// firmware update changes everything else about the device: RRF 3.6 presents one CDC port with
	// product id 0x60EE, RRF 3.7 presents two with 0x60EF, so the port name, the product id and
	// even the number of ports all change across an update. The socket does not.
	std::string topology_key;
	// Interface number of a composite device (the MI_xx part), or -1 when not composite. A Duet on
	// RRF 3.7 has two independent G-code channels; either works, and the lower is preferred.
	int         usb_interface = -1;
	// Windows CM_PROB_* code: 0 when healthy. A device whose driver failed to start is still
	// enumerated and still yields a port name, so without this a dead port looks like a live one.
	unsigned    problem_code = 0;

	SerialPortInfo() {}
	SerialPortInfo(std::string port) : port(port), friendly_name(std::move(port)) {}

	bool id_match(unsigned id_vendor, unsigned id_product) const { return id_vendor == this->id_vendor && id_product == this->id_product; }
};

inline bool operator==(const SerialPortInfo &sp1, const SerialPortInfo &sp2)
{
	return
		sp1.port       == sp2.port       &&
		sp1.id_vendor  == sp2.id_vendor  &&
		sp1.id_product == sp2.id_product &&
		sp1.is_printer == sp2.is_printer;
}

extern std::vector<std::string> 	scan_serial_ports();
extern std::vector<SerialPortInfo> 	scan_serial_ports_extended();

// USB vendor id shared by every Duet mainboard.
constexpr unsigned DUET_USB_VENDOR_ID = 0x1D50;

// Ports that look like a Duet mainboard, best candidate first: healthy before faulted, and the
// lower USB interface before the higher. On RRF 3.7 a board presents two CDC ports and both are
// full G-code channels, so either works.
extern std::vector<SerialPortInfo>	scan_duet_ports();


class Serial : public boost::asio::serial_port
{
public:
	Serial(boost::asio::io_context &io_service);
	Serial(boost::asio::io_context &io_service, const std::string &name, unsigned baud_rate);
	Serial(const Serial &) = delete;
	Serial &operator=(const Serial &) = delete;
	~Serial();

	void set_baud_rate(unsigned baud_rate);

	// The Serial implementation is currently in disarray and therefore commented out.
	// The boost implementation seems to have several problems, such as lack of support
	// for custom baud rates, few weird implementation bugs and a history of API breakages.
	// It's questionable whether it solves more problems than causes. Probably not.
	// TODO: Custom implementation not based on asio.
	//
	// As of now, this class is only kept for the purpose of rebooting AVR109,
	// see FirmwareDialog::priv::avr109_reboot()

/*
	void set_DTR(bool on);

	// Resets the line number both internally as well as with the firmware using M110
	void reset_line_num();

	// Reads a line or times out, the timeout is in milliseconds
	bool read_line(unsigned timeout, std::string &line, boost::system::error_code &ec);

	// Perform an initial setup for communicating with a printer
	void printer_setup();

	// Write data from a string
	size_t write_string(const std::string &str);

	// Attempts to reset the line numer and waits until the printer says "ok"
	bool printer_ready_wait(unsigned retries, unsigned timeout);

	// Write Marlin-formatted line, with a line number and a checksum
	size_t printer_write_line(const std::string &line, unsigned line_num);

	// Same as above, but with internally-managed line number
	size_t printer_write_line(const std::string &line);

	// Toggles DTR to reset the printer
	void printer_reset();

	// Formats a line Marlin-style, ie. with a sequential number and a checksum
	static std::string printer_format_line(const std::string &line, unsigned line_num);
private:
	unsigned m_line_num = 0;
*/
};


} // Utils
} // Slic3r

#endif /* slic3r_GUI_Utils_Serial_hpp_ */
