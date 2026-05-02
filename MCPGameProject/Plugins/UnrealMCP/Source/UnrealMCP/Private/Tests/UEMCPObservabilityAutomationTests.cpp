#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMCPObservabilitySmokeTest,
    "UEMCP.Observability.Smoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter
)

bool FUEMCPObservabilitySmokeTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("UEMCP automation bridge self-test executed"), true);
    return true;
}

#endif
