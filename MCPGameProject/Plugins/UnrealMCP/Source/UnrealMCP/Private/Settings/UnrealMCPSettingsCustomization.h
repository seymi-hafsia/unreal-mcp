#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FUnrealMCPSettingsCustomization : public IDetailCustomization
{
public:
        static TSharedRef<IDetailCustomization> MakeInstance();

        virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
        FReply OnTestConnection();
        FReply OnSendPing();
        FReply OnOpenLogsFolder();

        void ShowResultDialog(const FText& Message, bool bSuccess) const;
};
