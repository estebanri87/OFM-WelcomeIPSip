#pragma once
#include "WelcomeIPSipConfig.h"
#ifdef OPENKNX_WELCOMEIP_SIP

#include "OpenKNX.h"
#include <cstdint>
#include <string>

class WelcomeIPSipModule;

// One channel = one outdoor station. Reads the Link's ring table each loop() and maps it
// onto the channel's two KOs (Klingeln, Name der Station); never touches the SIP/pairing
// task directly, only the thread-safe snapshots the module hands it.
class WelcomeIPSipChannel : public OpenKNX::Channel
{
  public:
    enum ChannelType : uint8_t
    {
        TYPE_NONE = 0,
        TYPE_OUTDOOR = 1,
    };

    WelcomeIPSipChannel(uint8_t index, WelcomeIPSipModule &module);

    const std::string name() override;
    void setup() override;
    void loop() override;

    // 1-based, matches ParamWIS_CHStationNumber; 0 (inactive channel) never happens for
    // a channel object (createChannel() returns nullptr for TYPE_NONE).
    uint8_t stationNumber() const { return _stationNumber; }

  private:
    WelcomeIPSipModule &_module;
    uint8_t _type = TYPE_NONE;
    uint8_t _stationNumber = 1; // 1-based position in the ACL door list (ParamWIS_CHStationNumber)

    std::string _stationId; // resolved once from the door list; empty until then
    bool _nameSent = false;
    bool _warnedMissing = false;

    bool _ringActive = false;   // current level of the Klingeln KO (hold mode) / edge-armed (pulse mode)
    uint32_t _pulseStartedAtMs = 0;
};

#endif // OPENKNX_WELCOMEIP_SIP
