#include <sstream>
#include <string>
#include "ComputerCertificate.h"
#include "ErrorRecord.h"
#include "InstallInfo.h"
#include "MainDialog.h"
#include "StringUtility.h"

static std::wstring FormatErrorForPopup(const DWORD ErrorCode, const std::wstring& ErrorMessage, const std::wstring& Activity) noexcept
{
	return std::wstring{ L"Activity: " + Activity + L"\r\nErrorCode: " + std::to_wstring(ErrorCode) + L": " + ErrorMessage };
}

static std::wstring SplitForOutput(const std::vector<std::wstring>& SplitInput)
{
	std::wstring Output{};
	if (SplitInput.size() > 1)
	{
		for (auto const& InputLine : SplitInput)
		{
			Output.append(L"\r\n-  " + InputLine);
		}
	}
	else if (SplitInput.size() == 1)
		Output = L" " + SplitInput[0];
	Output.append(L"\r\n");
	return Output;
}

static std::wstring SplitForOutput(const std::wstring& GluePattern, const std::wstring& Composite)
{
	return SplitForOutput(SplitString(GluePattern, Composite));
}

INT_PTR CALLBACK MainDialog::SharedDialogProc(HWND hDialog, UINT uMessage, WPARAM wParam, LPARAM lParam)
{
	MainDialog* AppDialog{ nullptr };
	if (uMessage == WM_INITDIALOG)
	{
		AppDialog = (MainDialog*)lParam;
		AppDialog->DialogHandle = hDialog;
	}
	else
		AppDialog = (MainDialog*)GetWindowLongPtr(hDialog, GWL_USERDATA);

	if (AppDialog)
		return AppDialog->ThisDialogProc(uMessage, wParam, lParam);
	return FALSE;
}

INT_PTR CALLBACK MainDialog::ThisDialogProc(UINT uMessage, WPARAM wParam, LPARAM lParam)
{
	switch (uMessage)
	{
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			GetComputerCertificates();
			return TRUE;
		case IDCANCEL:
			SendMessage(DialogHandle, WM_CLOSE, 0, 0);
			return TRUE;
		case IDC_CERTLIST:
			if (HIWORD(wParam) == CBN_SELCHANGE)
				DisplayCertificate();
			break;
		case IDC_REFRESH:
			DisplayCertificateList();
			DetectWACInstallation();
			break;
		}
		break;
	case WM_INITDIALOG:
	{
		SetLastError(ERROR_SUCCESS);
		SetWindowLongPtr(DialogHandle, GWL_USERDATA, (LONG_PTR)this);
		if (auto LastError = GetLastError())
		{
			MessageBox(DialogHandle, FormatErrorForPopup(LastError, ErrorRecord::GetErrorMessage(LastError).c_str(), L"Setting application information").c_str(), L"Startup Error", MB_OK);
			PostQuitMessage(LastError);
		}
		else
		{
			auto InitResult = InitDialog();
			SetPictureBoxImage(IDC_ICONWACINSTALLED, InitResult == ERROR_SUCCESS);
			SendMessage(GetDlgItem(DialogHandle, IDC_RADIOSIMPLEPROGRESS), BM_SETCHECK, BST_CHECKED, NULL);
			DisplayCertificateList();
		}
	}
	break;
	case WM_CLOSE:
		DestroyWindow(DialogHandle);
		DeleteObject(TinyGreenBox);
		DeleteObject(TinyRedBox);
		return TRUE;
	case WM_DESTROY:
		PostQuitMessage(0);
		return TRUE;
	}
	return FALSE;
}

const DWORD MainDialog::DetectWACInstallation()
{
	auto[RegistryError, ModifyPath] = GetMSIModifyPath();
	auto ErrorCode = RegistryError.GetErrorCode();
	CmdlineModifyPath = ModifyPath;
	return ErrorCode;
}

void MainDialog::DisplayCertificateList() noexcept
{
	EnableDialogItem(IDC_CERTLIST, false);
	size_t ItemCount{ Certificates.size() };
	HWND CertList{ GetDlgItem(DialogHandle, IDC_CERTLIST) };
	SendMessage(CertList, CB_RESETCONTENT, 0, 0);

	if (ItemCount)
	{
		for (auto const& Certificate : Certificates)
		{
			SendMessage(CertList, CB_ADDSTRING, 0, (LPARAM)Certificate.FQDNFromSimpleRDN(Certificate.SubjectName()).c_str());
		}
		RECT CertListComboSize{ 0 };
		GetWindowRect(CertList, &CertListComboSize);
		SetWindowPos(CertList, 0, 0, 0, CertListComboSize.right - CertListComboSize.left,
			(SendMessage(CertList, CB_GETITEMHEIGHT, 0, 0) * ItemCount * 2), SWP_NOMOVE | SWP_NOZORDER);
		EnableDialogItem(IDC_CERTLIST, true);
	}
	else
		SendMessage(CertList, CB_ADDSTRING, 0, (LPARAM)L"No computer certificates found");
	SendMessage(CertList, CB_SETCURSEL, 0, 0);
	DisplayCertificate();
}

void MainDialog::DisplayCertificate()
{
	std::wstring CertificateText{};
	bool WACDetected{ CmdlineModifyPath.size() > 0 };
	bool CertValid{ false };
	bool ServerAuth{ false };
	bool PrivateKey{ false };

	HWND CertList{ GetDlgItem(DialogHandle, IDC_CERTLIST) };
	if (IsWindowEnabled(CertList) && Certificates.size())
	{
		ComputerCertificate& Certificate{ Certificates.at(SendMessage(CertList, CB_GETCURSEL, 0, 0)) };
		std::wstringstream CertificateDisplay{};
		CertificateDisplay << L"Subject:" << SplitForOutput(L", ?", Certificate.SubjectName());
		CertificateDisplay << L"Issuer: " << SplitForOutput(L", ?", Certificate.Issuer());
		CertificateDisplay << L"Subject Alternate Names:" << SplitForOutput(Certificate.SubjectAlternateNames());
		CertificateDisplay << L"Valid from: " << Certificate.ValidFrom() << L"\r\n";
		CertificateDisplay << L"Valid to: " << Certificate.ValidTo() << L"\r\n";
		CertificateDisplay << L"Thumbprint: " << Certificate.Thumbprint();
		CertificateDisplay.flush();
		CertificateText = CertificateDisplay.str();
		SetPictureBoxImage(IDC_ICONCERTVALID, (CertValid = Certificate.IsWithinValidityPeriod()));
		SetPictureBoxImage(IDC_ICONSERVAUTHALLOWED, (ServerAuth = Certificate.EKUServerAuthentication()));
		SetPictureBoxImage(IDC_ICONHASPRIVATEKEY, (PrivateKey = Certificate.HasPrivateKey()));
	}
	SetDlgItemText(DialogHandle, IDC_CERTDETAILS, CertificateText.c_str());
	EnableDialogItem(IDOK, WACDetected && CertValid && ServerAuth && PrivateKey);
}

void MainDialog::SetPictureBoxImage(const INT PictureBoxID, const bool Good)
{
	HBITMAP SelectedImage = Good ? TinyGreenBox : TinyRedBox;
	SendDlgItemMessage(DialogHandle, PictureBoxID, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)SelectedImage);
}

const DWORD MainDialog::InitDialog() noexcept
{
	HINSTANCE Instance{ GetModuleHandle(NULL) };
	TinyGreenBox = (HBITMAP)LoadImage(Instance, MAKEINTRESOURCE(IDB_TINYGREENBOX), IMAGE_BITMAP, 12, 12, LR_DEFAULTCOLOR);
	TinyRedBox = (HBITMAP)LoadImage(Instance, MAKEINTRESOURCE(IDB_TINYREDBOX), IMAGE_BITMAP, 12, 12, LR_DEFAULTCOLOR);
	SetPictureBoxImage(IDC_ICONWACINSTALLED, false);
	SetPictureBoxImage(IDC_ICONCERTVALID, false);
	SetPictureBoxImage(IDC_ICONSERVAUTHALLOWED, false);
	SetPictureBoxImage(IDC_ICONHASPRIVATEKEY, false);
	auto ErrorCode = DetectWACInstallation();
	return ErrorCode;
}

MainDialog::MainDialog(HINSTANCE Instance) : AppInstance(Instance)
{
	auto[CertLoadError, CertificateList] = GetComputerCertificates();
	Certificates = CertificateList;
	DialogHandle = CreateDialogParam(AppInstance, MAKEINTRESOURCE(IDD_MAIN), 0, &SharedDialogProc, (LPARAM)this);
}