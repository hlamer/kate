#ifndef SELECT_TARGET_DIALOG_H
#define SELECT_TARGET_DIALOG_H

#include <kdialog.h>

#include "plugin_katebuild.h"

#include <QStringList>

#include <map>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;


class SelectTargetDialog : public KDialog
{
    Q_OBJECT
    public:
        SelectTargetDialog(QList<KateBuildView::TargetSet>& targetSets, QWidget* parent);
        void setTargetSet(const QString& name);

        QString selectedTarget() const;

    protected:
        virtual bool eventFilter(QObject *obj, QEvent *event);

    private slots:
        void slotFilterTargets(const QString& filter);
        void slotCurrentItemChanged(QListWidgetItem* currentItem);
        void slotTargetSetSelected(int index);

    private:
        void setTargets(const std::map<QString, QString>& _targets);

        QStringList m_allTargets;

        QComboBox* m_currentTargetSet;
        QLineEdit* m_targetName;
        QListWidget* m_targetsList;
        QLabel* m_command;

        QList<KateBuildView::TargetSet>& m_targetSets;
        const std::map<QString, QString>* m_targets;
};

#endif

// kate: space-indent on; indent-width 4; replace-tabs on;