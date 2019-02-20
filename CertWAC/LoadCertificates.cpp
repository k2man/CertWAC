#include <Windows.h>
#include <wincrypt.h>
#include "ComputerCertificate.h"
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "NCrypt.lib")

const char* ServerAuthenticationOID{ "1.3.6.1.5.5.7.3.1" };

class CertificateStore
{
private:
	HCERTSTORE Store;
public:
	CertificateStore(HCERTSTORE NewStore) :Store(NewStore) {}
	~CertificateStore() { if (Store) CertCloseStore(Store, 0); }
	HCERTSTORE operator()() { return Store; }
	const bool IsValid() const { return Store != nullptr; }
};

enum class CertNames
{
	Subject,
	Issuer
};

static std::wstring GetNameFromCertificate(PCCERT_CONTEXT pCertContext, CertNames DesiredCertName)
{
	PCERT_NAME_BLOB DesiredName;
	switch (DesiredCertName)
	{
	case CertNames::Issuer:
		DesiredName = &pCertContext->pCertInfo->Issuer;
		break;
	default:
		DesiredName = &pCertContext->pCertInfo->Subject;
		break;
	}

	DWORD NameSize{ 0 };
	std::wstring CertName{ 0 };
	while (CertName[0] == 0)
	{
		NameSize = CertNameToStr(pCertContext->dwCertEncodingType, DesiredName,
			CERT_OID_NAME_STR, CertName.data(), NameSize);
		if (CertName.size() < NameSize)
			CertName.reserve(NameSize);
	}
	return CertName;
}

std::pair<ErrorRecord, std::vector<ComputerCertificate>> GetComputerCertificates()
{
	const wchar_t* StoreName{ L"MY" };
	std::vector<ComputerCertificate> Certificates{};

	auto ComputerStore = CertificateStore(
		CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, NULL, CERT_SYSTEM_STORE_LOCAL_MACHINE, StoreName));

	if (!ComputerStore.IsValid())
	{
		return std::make_pair(ErrorRecord(GetLastError(), L"Opening computer certificate store"), Certificates);
	}

	PCCERT_CONTEXT pCertContext{ nullptr };
	while (pCertContext = CertEnumCertificatesInStore(ComputerStore(), pCertContext))
	{
		DWORD CertHashSize{ 0 };
		if (CryptHashCertificate2(BCRYPT_SHA1_ALGORITHM, 0, NULL,
			pCertContext->pbCertEncoded, pCertContext->cbCertEncoded, NULL, &CertHashSize))
		{
			ComputerCertificate ThisCert{};
			ThisCert.SubjectName = GetNameFromCertificate(pCertContext, CertNames::Subject);
			ThisCert.Issuer = GetNameFromCertificate(pCertContext, CertNames::Issuer);
			ThisCert.ValidFrom = pCertContext->pCertInfo->NotBefore;
			ThisCert.ValidTo = pCertContext->pCertInfo->NotAfter;

			// Enhanced Key Usage
			DWORD EnhancedKeyUsageSize{ 0 };
			if (CertGetEnhancedKeyUsage(pCertContext, 0, NULL, &EnhancedKeyUsageSize))
			{
				PCERT_ENHKEY_USAGE pKeyEnhancedKeyUsage{ (CERT_ENHKEY_USAGE*)(new char[EnhancedKeyUsageSize]) };
				if (CertGetEnhancedKeyUsage(pCertContext, 0, pKeyEnhancedKeyUsage, &EnhancedKeyUsageSize))
				{
					for (auto i{ 0 }; i != pKeyEnhancedKeyUsage->cUsageIdentifier; ++i)
					{
						if (std::string{ ServerAuthenticationOID } == std::string{ pKeyEnhancedKeyUsage->rgpszUsageIdentifier[i] })
						{
							ThisCert.ServerAuthentication = true;
						}
					}

				}
				delete[] pKeyEnhancedKeyUsage;
			}

			// SAN
			PCERT_EXTENSION Extension = nullptr;
			PCERT_ALT_NAME_INFO pAlternateNameInfo = nullptr;
			PCERT_ALT_NAME_ENTRY pAlternateNameEntry = nullptr;
			DWORD dwAlternateNameInfoSize;

			std::string ExtensionOID{};
			for (auto i{ 0 }; i != pCertContext->pCertInfo->cExtension; ++i)
			{
				Extension = &pCertContext->pCertInfo->rgExtension[i];
				ExtensionOID.assign(Extension->pszObjId);
				if (ExtensionOID == szOID_SUBJECT_ALT_NAME || ExtensionOID == szOID_SUBJECT_ALT_NAME2)
				{
					DWORD DataSize{ 0 };
					if (CryptFormatObject(pCertContext->dwCertEncodingType, 0, 0, NULL, szOID_SUBJECT_ALT_NAME2,
						Extension->Value.pbData, Extension->Value.cbData, NULL, &DataSize))
					{
						std::wstring AlternateName{};
						AlternateName.reserve(DataSize);
						if (CryptFormatObject(pCertContext->dwCertEncodingType, 0, 0, NULL, szOID_SUBJECT_ALT_NAME2,
							Extension->Value.pbData, Extension->Value.cbData,
							(void*)AlternateName.data(), &DataSize))
						{
							ThisCert.SubjectAlternateNames.emplace_back(AlternateName);
						}
					}
				}
			}

			// thumbprint
			ThisCert.Thumbprint.reserve(CertHashSize);
			CryptHashCertificate2(BCRYPT_SHA1_ALGORITHM, 0, NULL, pCertContext->pbCertEncoded,
				pCertContext->cbCertEncoded, ThisCert.Thumbprint.data(), &CertHashSize);

			// detect private key
			HCRYPTPROV_OR_NCRYPT_KEY_HANDLE PrivateKeyHandle;
			DWORD PrivateKeyInfo{ 0 };
			DWORD KeySpec{ 0 };
			BOOL MustFree{ FALSE };
			if (CryptAcquireCertificatePrivateKey(pCertContext, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG, NULL, &PrivateKeyHandle, &KeySpec, &MustFree))
			{
				ThisCert.PrivateKey = true;
				if (MustFree)
				{
					if (KeySpec == CERT_NCRYPT_KEY_SPEC) { NCryptFreeObject(PrivateKeyHandle); }
					else { CryptReleaseContext(PrivateKeyHandle, 0); }
				}
			}
			Certificates.emplace_back(ThisCert);
		}
	}

	if (pCertContext != nullptr)
		CertFreeCertificateContext(pCertContext);

	return std::make_pair(ErrorRecord(ERROR_SUCCESS, L"Reading certificates"), Certificates);
}