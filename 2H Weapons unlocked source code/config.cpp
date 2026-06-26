#include "config.h"

namespace BarebonesVR {
		
	int logging = 0;
    int leftHandedMode = 0;

    void loadConfig() 
    {
        std::string runtimeDirectory = GetRuntimeDirectory();

        if (!runtimeDirectory.empty()) 
        {
            std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\2hWeaponsUnlocked.ini";
            std::ifstream file(filepath);

            if (!file.is_open()) 
            {
                transform(filepath.begin(), filepath.end(), filepath.begin(), ::tolower);
                file.open(filepath);
            }

            if (file.is_open()) 
            {
                std::string line;
                std::string currentSection;

                while (std::getline(file, line)) 
                {
                    trim(line);
                    skipComments(line);

                    if (line.empty()) continue;

                    if (line[0] == '[') 
                    {
                        // New section
                        size_t endBracket = line.find(']');
                        if (endBracket != std::string::npos) 
                        {
                            currentSection = line.substr(1, endBracket - 1);
                            trim(currentSection);                            
                        }
                    }
                    else if (currentSection == "Settings") 
                    {
                        std::string variableName;
                        std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

                        if (variableName == "Logging") 
                        {
                            logging = std::stoi(variableValueStr);
                        }
                    }                    
                } 
            }
            _MESSAGE("Config file is loaded successfully.");
            return;
        }
        return;
    }

	void Log(const int msgLogLevel, const char* fmt, ...)
	{
		if (msgLogLevel > logging)
		{
			return;
		}

		va_list args;
		char logBuffer[4096];

		va_start(args, fmt);
		vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
		va_end(args);

		_MESSAGE(logBuffer);
	}

}