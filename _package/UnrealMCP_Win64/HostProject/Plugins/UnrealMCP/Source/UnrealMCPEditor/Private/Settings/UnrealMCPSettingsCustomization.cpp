#include "Settings/UnrealMCPSettingsCustomization.h"
#include "CoreMinimal.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "Settings/UnrealMCPDiagnostics.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPSettingsCustomization"

TSharedRef<IDetailCustomization> FUnrealMCPSettingsCustomization::MakeInstance()
{
        return MakeShareable(new FUnrealMCPSettingsCustomization());
}

void FUnrealMCPSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
        IDetailCategoryBuilder& DiagnosticsCategory = DetailBuilder.EditCategory(TEXT("Diagnostics"));

        DiagnosticsCategory.AddCustomRow(LOCTEXT("DiagnosticsActions", "Diagnostics"))
        .WholeRowWidget
        [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.f, 0.f, 8.f, 0.f)
                [
                        SNew(SButton)
                        .Text(LOCTEXT("TestConnectionButton", "Test Connection"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnTestConnection)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.f, 0.f, 8.f, 0.f)
                [
                        SNew(SButton)
                        .Text(LOCTEXT("SendPingButton", "Send Ping"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnSendPing)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                        SNew(SButton)
                        .Text(LOCTEXT("OpenLogsButton", "Open Logs Folder"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnOpenLogsFolder)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                        SNew(SButton)
                        .Text(LOCTEXT("OpenEventsLogButton", "Open Events Log"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnOpenEventsLog)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                        SNew(SButton)
                        .Text(LOCTEXT("OpenMetricsLogButton", "Open Metrics Log"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnOpenMetricsLog)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                        SNew(SButton)
                        .Text(LOCTEXT("TailLogsButton", "Tail Logs"))
                        .OnClicked(this, &FUnrealMCPSettingsCustomization::OnTailLogs)
                ]
        ];
}

FReply FUnrealMCPSettingsCustomization::OnTestConnection()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::TestConnection(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

FReply FUnrealMCPSettingsCustomization::OnSendPing()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::SendPing(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

FReply FUnrealMCPSettingsCustomization::OnOpenLogsFolder()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::OpenLogsFolder(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

FReply FUnrealMCPSettingsCustomization::OnOpenEventsLog()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::OpenEventsLog(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

FReply FUnrealMCPSettingsCustomization::OnOpenMetricsLog()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::OpenMetricsLog(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

FReply FUnrealMCPSettingsCustomization::OnTailLogs()
{
        FText Message;
        const bool bSuccess = FUnrealMCPDiagnostics::TailLogs(Message);
        ShowResultDialog(Message, bSuccess);
        return FReply::Handled();
}

void FUnrealMCPSettingsCustomization::ShowResultDialog(const FText& Message, bool bSuccess) const
{
        const FText Title = bSuccess ? LOCTEXT("DiagnosticsSuccessTitle", "Diagnostics") : LOCTEXT("DiagnosticsFailureTitle", "Diagnostics Error");
        FMessageDialog::Open(EAppMsgType::Ok, Message, &Title);
}

#undef LOCTEXT_NAMESPACE
